/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * ArduPilot device driver for SLAMTEC RPLIDAR A2 (16m range version)
 *
 * ALL INFORMATION REGARDING PROTOCOL WAS DERIVED FROM RPLIDAR DATASHEET:
 *
 * https://www.slamtec.com/en/Lidar
 * http://bucket.download.slamtec.com/63ac3f0d8c859d3a10e51c6b3285fcce25a47357/LR001_SLAMTEC_rplidar_protocol_v1.0_en.pdf
 *
 * Author: Steven Josefs, IAV GmbH
 * Based on the LightWare SF40C ArduPilot device driver from Randy Mackay
 *
 */


#include <AP_HAL/AP_HAL.h>
#include "AP_Proximity_RPLidarA2.h"
#include <AP_SerialManager/AP_SerialManager.h>
#include <ctype.h>
#include <stdio.h>

#define RP_DEBUG_LEVEL 0

#if RP_DEBUG_LEVEL
  #include <GCS_MAVLink/GCS.h>
  #define Debug(level, fmt, args ...)  do { if (level <= RP_DEBUG_LEVEL) { gcs().send_text(MAV_SEVERITY_INFO, fmt, ## args); } } while (0)
#else
  #define Debug(level, fmt, args ...)
#endif

#define COMM_ACTIVITY_TIMEOUT_MS        200
#define RESET_RPA2_WAIT_MS              8
#define RESYNC_TIMEOUT                  5000



extern const AP_HAL::HAL& hal;




/**************************************************************************************************************
*函数原型：
*函数功能：构造函数：初始化近距离传感器，注意这个构造函数不能被号召删除，因此我们总是初始化近距离传感器
*修改日期：2019-3-21
*修改作者：
*备注信息：The constructor also initialises the proximity sensor. Note that this
         constructor is not called until detect() returns true, so we
         already know that we should setup the proximity sensor
****************************************************************************************************************/

AP_Proximity_RPLidarA2::AP_Proximity_RPLidarA2(AP_Proximity &_frontend,
                                               AP_Proximity::Proximity_State &_state,
                                               AP_SerialManager &serial_manager) :
                                               AP_Proximity_Backend(_frontend, _state)
{
    _uart = serial_manager.find_serial(AP_SerialManager::SerialProtocol_Lidar360, 0); //查找串口
    if (_uart != nullptr)
    {
        _uart->begin(serial_manager.find_baudrate(AP_SerialManager::SerialProtocol_Lidar360, 0));
    }
    _cnt = 0 ;         //计数等于0
    _sync_error = 0 ;  //同步信息错误
    _byte_count = 0;   //位计数
}


/**************************************************************************************************************
*函数原型：bool AP_Proximity_RPLidarA2::detect(AP_SerialManager &serial_manager)
*函数功能：识别传感器数据
*修改日期：2019-3-19
*修改作者：
*备注信息：
****************************************************************************************************************/
bool AP_Proximity_RPLidarA2::detect(AP_SerialManager &serial_manager)
{
    return serial_manager.find_serial(AP_SerialManager::SerialProtocol_Lidar360, 0) != nullptr;
}


/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::update(void)
*函数功能：数据更新
*修改日期：2019-2-18
*修改作者：
*备注信息：update the _rp_state of the sensor
****************************************************************************************************************/
void AP_Proximity_RPLidarA2::update(void)
{
    if (_uart == nullptr)
    {
        return;
    }

    //如果有必要的话，初始化传感器-----initialise sensor if necessary
    if (!_initialised)
    {
        _initialised = initialise();    //如果所有的初始化合适，返回1-----returns true if everything initialized properly
    }

    //识别到LIDAR----- if LIDAR in known state
    if (_initialised)
    {
        get_readings();  //读取数据
    }

    // check for timeout and set health status
    if ((_last_distance_received_ms == 0) || (AP_HAL::millis() - _last_distance_received_ms > COMM_ACTIVITY_TIMEOUT_MS))
    {
        set_status(AP_Proximity::Proximity_NoData);
        Debug(1, "LIDAR NO DATA");
    } else
    {
        set_status(AP_Proximity::Proximity_Good);
    }
}


/**************************************************************************************************************
*函数原型：float AP_Proximity_RPLidarA2::distance_max() const
*函数功能：最大是16m
*修改日期：2019-3-22
*修改作者：
*备注信息： get maximum distance (in meters) of sensor
****************************************************************************************************************/
float AP_Proximity_RPLidarA2::distance_max() const
{
    return 16.0f;  //16m max range RPLIDAR2, if you want to support the 8m version this is the only line to change
}


/**************************************************************************************************************
*函数原型：float AP_Proximity_RPLidarA2::distance_min() const
*函数功能：最小距离20cm
*修改日期：2019-2-18
*修改作者：
*备注信息：get minimum distance (in meters) of sensor
****************************************************************************************************************/
float AP_Proximity_RPLidarA2::distance_min() const
{
    return 0.20f;  //20cm min range RPLIDAR2
}


/**************************************************************************************************************
*函数原型：bool AP_Proximity_RPLidarA2::initialise()
*函数功能：初始化传感器
*修改日期：2019-2-18
*修改作者：
*备注信息：
****************************************************************************************************************/
bool AP_Proximity_RPLidarA2::initialise()
{
    //初始化区域----initialise sectors
    if (!_sector_initialised)
    {
        init_sectors();
        return false;
    }
    if (!_initialised)
    {
        reset_rplidar();            // set to a known state
        Debug(1, "LIDAR initialised");
        return true;
    }

    return true;
}

/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::reset_rplidar()
*函数功能：测距核心软重启命令请求
*修改日期：2019-2-18
*修改作者：
*备注信息：
****************************************************************************************************************/
void AP_Proximity_RPLidarA2::reset_rplidar()
{
    if (_uart == nullptr) {
        return;
    }
    uint8_t tx_buffer[2] = {RPLIDAR_PREAMBLE, RPLIDAR_CMD_RESET}; //0xA5,0X40
    _uart->write(tx_buffer, 2);
    _resetted = true;   ///< be aware of extra 63 bytes coming after reset containing FW information
    Debug(1, "LIDAR reset");
    // To-Do: ensure delay of 8m after sending reset request
    _last_reset_ms =  AP_HAL::millis();
    _rp_state = rp_resetted;

}


/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::init_sectors()
*函数功能：使用用户定义的忽略区域初始化扇区角度，左对齐sf40c
*修改日期：2019-2-18
*修改作者：
*备注信息：initialise sector angles using user defined ignore areas, left same as SF40C
****************************************************************************************************************/
void AP_Proximity_RPLidarA2::init_sectors()
{
    //如果未定义忽略区域，则使用默认值---------use defaults if no ignore areas defined
    const uint8_t ignore_area_count = get_ignore_area_count();
    if (ignore_area_count == 0)
    {
        _sector_initialised = true;
        return;
    }

    uint8_t sector = 0;
    for (uint8_t i=0; i<ignore_area_count; i++)
    {

        //获取忽略区域信息------ get ignore area info
        uint16_t ign_area_angle;
        uint8_t ign_area_width;
        if (get_ignore_area(i, ign_area_angle, ign_area_width))
        {

            // calculate how many degrees of space we have between this end of this ignore area and the start of the end
        	//计算在这个忽略区域的这一端和这一端的开始之间有多少度的空间
            int16_t start_angle, end_angle;
            get_next_ignore_start_or_end(1, ign_area_angle, start_angle);
            get_next_ignore_start_or_end(0, start_angle, end_angle);
            int16_t degrees_to_fill = wrap_360(end_angle - start_angle);

            // divide up the area into sectors
            while ((degrees_to_fill > 0) && (sector < PROXIMITY_SECTORS_MAX))
            {
                uint16_t sector_size;
                if (degrees_to_fill >= 90)
                {
                    // set sector to maximum of 45 degrees
                    sector_size = 45;
                } else if (degrees_to_fill > 45)
                {
                    // use half the remaining area to optimise size of this sector and the next
                    sector_size = degrees_to_fill / 2.0f;
                } else
                {
                    // 45 degrees or less are left so put it all into the next sector
                    sector_size = degrees_to_fill;
                }
                // record the sector middle and width
                _sector_middle_deg[sector] = wrap_360(start_angle + sector_size / 2.0f);
                _sector_width_deg[sector] = sector_size;

                // move onto next sector
                start_angle += sector_size;
                sector++;
                degrees_to_fill -= sector_size;
            }
        }
    }

    // set num sectors
    _num_sectors = sector;

    // re-initialise boundary because sector locations have changed
    init_boundary();

    // record success
    _sector_initialised = true;
}


/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::set_scan_mode()
*函数功能：开始扫描采样命令请求与回应数据格式
*修改日期：2019-3-22
*修改作者：
*备注信息：set Lidar into SCAN mode
****************************************************************************************************************/
void AP_Proximity_RPLidarA2::set_scan_mode()
{
    if (_uart == nullptr)
    {
        return;
    }
    uint8_t tx_buffer[2] = {RPLIDAR_PREAMBLE, RPLIDAR_CMD_SCAN};
    _uart->write(tx_buffer, 2);
    _last_request_ms = AP_HAL::millis();
    Debug(1, "LIDAR SCAN MODE ACTIVATED");
    _rp_state = rp_responding;
}

/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::send_request_for_health()                         //not called yet
*函数功能：发送设备健康状态获取命令请求
*修改日期：2019-3-22
*修改作者：
*备注信息：send request for sensor health
****************************************************************************************************************/
void AP_Proximity_RPLidarA2::send_request_for_health()
{
    if (_uart == nullptr)
    {
        return;
    }
    uint8_t tx_buffer[2] = {RPLIDAR_PREAMBLE, RPLIDAR_CMD_GET_DEVICE_HEALTH}; //0xA5,0X52
    _uart->write(tx_buffer, 2);
    _last_request_ms = AP_HAL::millis();
    _rp_state = rp_health;
}

/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::get_readings()
*函数功能：读取数据
*修改日期：2019-3-21
*修改作者：
*备注信息：
****************************************************************************************************************/
void AP_Proximity_RPLidarA2::get_readings()
{
    if (_uart == nullptr)
    {
        return;
    }
    Debug(2, "             CURRENT STATE: %d ", _rp_state);
    uint32_t nbytes = _uart->available();

    while (nbytes-- > 0)  //串口数据是否有效
    {

        uint8_t c = _uart->read();  //读取串口数据
        Debug(2, "UART READ %x <%c>", c, c); //show HEX values

        STATE:
        switch(_rp_state)  //根据_rp_state来进入不同的流程
        {

            case rp_resetted:          //1
                Debug(3, "                  BYTE_COUNT %d", _byte_count);
                if ((c == 0x52 || _information_data) && _byte_count < 62) //_information_data刚开始是0，_byte_count字节计数小于62
                {
                    if (c == 0x52) //确实雷达是健康的
                    {
                        _information_data = true;  //设定_information_data=1
                    }
                    _rp_systeminfo[_byte_count] = c;
                    Debug(3, "_rp_systeminfo[%d]=%x",_byte_count,_rp_systeminfo[_byte_count]);
                    _byte_count++;
                    break;
                } else
                {

                    if (_information_data)
                    {
                        Debug(1, "GOT RPLIDAR INFORMATION");
                        _information_data = false;
                        _byte_count = 0;
                        set_scan_mode();
                        break;
                    }

                    if (_cnt>5)
                    {
                        _rp_state = rp_unknown;
                        _cnt=0;
                        break;
                    }
                    _cnt++;
                    break;
                }
                break;

            case rp_responding:  //2
                Debug(2, "RESPONDING");
                if (c == RPLIDAR_PREAMBLE || _descriptor_data) //获取响应，首先收到的是0xA5,_descriptor_data=0
                {
                    _descriptor_data = true;
                    _descriptor[_byte_count] = c;  //_byte_count=0,1,2,3,4,5,6
                    _byte_count++;                 //等于7时
                    //描述符包总共有7个字节----------descriptor packet has 7 byte in total
                    if (_byte_count == sizeof(_descriptor)) //sizeof(_descriptor)=7
                    {
                        Debug(2,"LIDAR DESCRIPTOR CATCHED");
                        _response_type = ResponseType_Descriptor;
                        //在描述符之后标识有效负载数据--- identify the payload data after the descriptor
                        parse_response_descriptor();
                        _byte_count = 0;
                    }
                } else
                {
                    _rp_state = rp_unknown;
                }
                break;

            case rp_measurements:  //3
                if (_sync_error)
                {
                    // out of 5-byte sync mask -> catch new revolution
                    Debug(1, "       OUT OF SYNC");
                    // on first revolution bit 1 = 1, bit 2 = 0 of the first byte
                    if ((c & 0x03) == 0x01)
                    {
                        _sync_error = 0;
                        Debug(1, "                  RESYNC");
                    } else
                    {
                        if (AP_HAL::millis() - _last_distance_received_ms > RESYNC_TIMEOUT)
                        {
                            reset_rplidar();
                        }
                        break;
                    }
                }
                Debug(3, "READ PAYLOAD");
                payload[_byte_count] = c;
                _byte_count++;

                if (_byte_count == _payload_length)
                {
                    Debug(2, "LIDAR MEASUREMENT CATCHED");
                    parse_response_data();
                    _byte_count = 0;
                }
                break;

            case rp_health:  //4
                Debug(1, "state: HEALTH");
                break;

            case rp_unknown:  //0
                Debug(1, "state: UNKNOWN");
                if (c == RPLIDAR_PREAMBLE)
                {
                    _rp_state = rp_responding;
                    goto STATE;
                    break;
                }
                _cnt++;
                if (_cnt>10)
                {
                    reset_rplidar();
                    _rp_state = rp_resetted;
                    _cnt=0;
                }
                break;

            default:
                Debug(1, "UNKNOWN LIDAR STATE");
                break;
        }
    }
}
/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::parse_response_descriptor()
*函数功能：解析数据描述
*修改日期：2019-2-18
*修改作者：
*备注信息：
****************************************************************************************************************/
void AP_Proximity_RPLidarA2::parse_response_descriptor()
{
    // check if descriptor packet is valid
    if (_descriptor[0] == RPLIDAR_PREAMBLE && _descriptor[1] == 0x5A)
    {

        if (_descriptor[2] == 0x05 && _descriptor[3] == 0x00 && _descriptor[4] == 0x00 && _descriptor[5] == 0x40 && _descriptor[6] == 0x81)
        {
            // payload is SCAN measurement data
            _payload_length = sizeof(payload.sensor_scan);
            static_assert(sizeof(payload.sensor_scan) == 5, "Unexpected payload.sensor_scan data structure size");
            _response_type = ResponseType_SCAN;
            Debug(2, "Measurement response detected");
            _last_distance_received_ms = AP_HAL::millis();
            _rp_state = rp_measurements;
        }
        if (_descriptor[2] == 0x03 && _descriptor[3] == 0x00 && _descriptor[4] == 0x00 && _descriptor[5] == 0x00 && _descriptor[6] == 0x06) {
            // payload is health data
            _payload_length = sizeof(payload.sensor_health);
            static_assert(sizeof(payload.sensor_health) == 3, "Unexpected payload.sensor_health data structure size");
            _response_type = ResponseType_Health;
            _last_distance_received_ms = AP_HAL::millis();
            _rp_state= rp_health;
        }
        return;
    }
    Debug(1, "Invalid response descriptor");
    _rp_state = rp_unknown;
}

/**************************************************************************************************************
*函数原型：void AP_Proximity_RPLidarA2::parse_response_data()
*函数功能：数据更新
*修改日期：2019-3-21
*修改作者：
*备注信息：解析响应数据
****************************************************************************************************************/
//int time_lidarA2=0;
//float angle_lidarA2,distance_lidarA2;//测试用
float lidarA2_test[9];
void AP_Proximity_RPLidarA2::parse_response_data()
{
float D2R=0.017453;//角度转成弧度
//bool front_obstacle=0;
//bool back_obstacle=0;

    switch (_response_type){
        case ResponseType_SCAN:
            Debug(2, "UART %02x %02x%02x %02x%02x", payload[0], payload[2], payload[1], payload[4], payload[3]); //show HEX values
            // check if valid SCAN packet: a valid packet starts with startbits which are complementary plus a checkbit in byte+1
            if ((payload.sensor_scan.startbit == !payload.sensor_scan.not_startbit) && payload.sensor_scan.checkbit) {
                const float angle_deg = payload.sensor_scan.angle_q6/64.0f;
                const float distance_m = (payload.sensor_scan.distance_q2/4000.0f);

                //此雷达在扇区切换处容易出问题，360度，180度处
                //处理数据，获取前方障碍物模型，假设前方障碍物为一条直线
                //获得直线的距离，方位，从而计算出需要避障的距离和方位
                //首先计算机头方向
                //寻找前方障碍物的最大角度和最小角度
               //右边切换处，有时数据会雷同，左右一样，风险，当心！！！
                if(angle_deg>=270&&angle_deg<=360)
                {
                	if(distance_m>=0.5)
                	{
                		if(!point_front_find)
                		{
                		angle_left_front=angle_deg;
                		distant_left_front=distance_m;
                		point_front_find=1;
                		}
                		angle_right_front=angle_deg;
                		distant_right_front=distance_m;
                	}
                }
                //圈外
                else
                {

                	if(angle_deg<60)
                	{

                	if(point_front_find)
                	{
                	//前方有障碍物
                	//front_obstacle=1;
                	//point_front_find=0;
                	L_left_front=distant_left_front*sinf(D2R*(angle_left_front-315));
                	L_right_front=distant_right_front*sinf(D2R*(angle_right_front-315));

                	//测量数据存在噪声，有可能导致左右数据相等
                	if(L_left_front!=L_right_front)
                	{

                	distance_object_front=(distant_left_front*cosf(D2R*(angle_left_front-315))+distant_right_front*cosf(D2R*(angle_right_front-315)))/2;

                	if(L_left_front>=0&&L_right_front>=0)
                	                	{
                	                		avoid_distance_front=500-abs(L_left_front*100);//转换成cm
                	                		avoid_direction_front=-1;//往左边飞
                	                	}

                	                	if(L_left_front<0&&L_right_front>0)
                	                	{
                	                		//abs 会转成int类型，小心！！！
                	                		if(abs(L_left_front*10)>abs(L_right_front*10))
                	                		{
                	                			avoid_distance_front=500+abs(L_right_front*100);//转换成cm
                	                			avoid_direction_front=1;//往右边飞
                	                		}
                	                		else
                	                		{
                	                			avoid_distance_front=500+abs(L_left_front*100);//转换成cm
                	                			avoid_direction_front=-1;//往左边飞

                	                		}

                	                	}

                	                	if(L_left_front<0&&L_right_front<0)
                	                     {
                	                		avoid_distance_front=500-abs(L_right_front*100);//转换成cm
                	                		avoid_direction_front=1;//往右边飞
                	                      }

                	}

                	}

                	else//数值大，障碍物距离很远
                		distance_object_front=133;

                	}

                	else
                		point_front_find=0;


                	//危险，小心，关掉障碍物标志后，else要关掉

                	//else
                	//distance_object_front=133;


                		//front_obstacle=0;



                	lidarA2_test[0]=angle_left_front;
                	lidarA2_test[1]=distant_left_front;
                	lidarA2_test[2]=angle_right_front;
                	lidarA2_test[3]=distant_right_front;

                	lidarA2_test[4]=avoid_distance_front;
                	lidarA2_test[5]=avoid_direction_front;
                	lidarA2_test[6]=distance_object_front;

                	lidarA2_test[7]=L_left_front;
                	lidarA2_test[8]=L_right_front;



                }


                                //计算机尾方向
                                //寻找后方障碍物的最大角度和最小角度
                                if(angle_deg>=90&&angle_deg<=180)
                                {
                                	if(distance_m>=0.5)
                                	{
                                		if(!point_back_find)
                                		{
                                			//方向反了 前 后方的数据很多地方相反，风险，当心！！！
                                		angle_right_back=angle_deg;
                                		distant_right_back=distance_m;

                                		point_back_find=1;
                                		}
                                		angle_left_back=angle_deg;
                                		distant_left_back=distance_m;
                                	}
                                }
                                //圈外
                                else
                                {

                                	if(angle_deg>180)
                                	{

                                	if(point_back_find)
                                	{
                                	//后方有障碍物
                                	//back_obstacle=1;
                                	//point_back_find=0;
                                	L_left_back=distant_left_back*sinf(D2R*(angle_left_back-135));
                                    L_right_back=distant_right_back*sinf(D2R*(angle_right_back-135));

                                    //测量数据存在噪声，有可能导致左右数据相等
                                    if(L_left_back!=L_right_back)
                                    {

                                    distance_object_back=(distant_left_back*cosf(D2R*(angle_left_back-135))+distant_right_back*cosf(D2R*(angle_right_back-135)))/2;

                                    if(L_left_back>=0&&L_right_back>=0)
                                    {
                                     avoid_distance_back=500-abs(L_right_back*100);//转换成cm
                                     avoid_direction_back=1;//往右边边飞
                                      }

                                      //和前方障碍物方向相反，请注意！！！
                                      if(L_left_back>0&&L_right_back<0)
                                       {
                                       if(abs(L_left_back*10)>abs(L_right_back*10))
                                       {
                                       avoid_distance_back=500+abs(L_right_back*100);//转换成cm
                                       avoid_direction_back=1;//往右边飞
                                       }
                                       else
                                       {
                                       avoid_distance_back=500+abs(L_left_back*100);//转换成cm
                                       avoid_direction_back=-1;//往左边飞

                                       }
                                        }

                                        if(L_left_back<0&&L_right_back<0)
                                        {
                                         avoid_distance_back=500-abs(L_left_back*100);//转换成cm
                                         avoid_direction_back=-1;//往左边飞
                                          }
                                    }
                                	}

                                	//数值大，障碍物距离很远
                                	 else
                                	distance_object_back=133;

                                	}


                                	else
                                		point_back_find=0;
                                	//	back_obstacle=0;

/*
                                	lidarA2_test[0]=angle_left_back;
                                	lidarA2_test[1]=distant_left_back;
                                	lidarA2_test[2]=angle_right_back;
                                	lidarA2_test[3]=distant_right_back;

                                	lidarA2_test[4]=avoid_distance_back;
                                	lidarA2_test[5]=avoid_direction_back;
                                	lidarA2_test[6]=distance_object_back;

                                	lidarA2_test[7]=L_left_back;
                                	lidarA2_test[8]=L_right_back;

*/
                                }


                //测试用
                //angle_lidarA2=angle_deg;
               // distance_lidarA2=distance_m;
                /*
                time_lidarA2++;
                if(time_lidarA2>100)
                {
                	time_lidarA2=0;
                    gcs().send_text(MAV_SEVERITY_INFO, "time_lidarA2: angle_deg=%f distance_m=%f", (double)angle_deg, (double)distance_m);

                }
*/
                #if RP_DEBUG_LEVEL >= 2
                const float quality = payload.sensor_scan.quality;
                Debug(2, "                                       D%02.2f A%03.1f Q%02d", distance_m, angle_deg, quality);
#endif
                _last_distance_received_ms = AP_HAL::millis();
                uint8_t sector;
                if (convert_angle_to_sector(angle_deg, sector)) {
                    if (distance_m > distance_min()) {
                        if (_last_sector == sector) {
                            if (_distance_m_last > distance_m) {
                                _distance_m_last = distance_m;
                                _angle_deg_last  = angle_deg;
                            }
                        } else {
                            // a new sector started, the previous one can be updated now
                            _angle[_last_sector] = _angle_deg_last;
                            _distance[_last_sector] = _distance_m_last;
                            _distance_valid[_last_sector] = true;
                            // update boundary used for avoidance
                            update_boundary_for_sector(_last_sector);
                            // initialize the new sector
                            _last_sector     = sector;
                            _distance_m_last = distance_m;
                            _angle_deg_last  = angle_deg;
                        }
                    } else {
                        _distance_valid[sector] = false;
                    }
                }
            } else {
                // not valid payload packet
                Debug(1, "Invalid Payload");
                _sync_error++;
            }
            break;

        case ResponseType_Health:
            // health issue if status is "3" ->HW error
            if (payload.sensor_health.status == 3) {
                Debug(1, "LIDAR Error");
            }
            break;

        default:
            // no valid payload packets recognized: return payload data=0
            Debug(1, "Unknown LIDAR packet");
            break;
    }
}


/**************************************************************************************************************
*                                      File-End
****************************************************************************************************************/
