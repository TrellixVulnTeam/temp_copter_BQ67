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


#pragma once

#include "AP_Proximity.h"
#include "AP_Proximity_Backend.h"
#include <AP_HAL/AP_HAL.h>                   ///< for UARTDriver

// Commands
//-----------------------------------------

// Commands without payload and response
#define RPLIDAR_PREAMBLE               0xA5   //起始帧头命令
#define RPLIDAR_CMD_STOP               0x25   //阻止帧头命令
#define RPLIDAR_CMD_SCAN               0x20   //请求进入扫描状态命令
#define RPLIDAR_CMD_FORCE_SCAN         0x21   //请求进入扫描状态命令，强制数据输出
#define RPLIDAR_CMD_RESET              0x40   //阻止帧头命令

// Commands without payload but have response
#define RPLIDAR_CMD_GET_DEVICE_INFO    0x50   //单次获取设备序列号等信息
#define RPLIDAR_CMD_GET_DEVICE_HEALTH  0x52   //获取设备健康

// Commands with payload and have response
#define RPLIDAR_CMD_EXPRESS_SCAN       0x82   //多次请求进入扫描采样状态，并工作在最高采样频率下




class AP_Proximity_RPLidarA2 : public AP_Proximity_Backend
{

public:
    // constructor
    AP_Proximity_RPLidarA2(AP_Proximity &_frontend, AP_Proximity::Proximity_State &_state, AP_SerialManager &serial_manager);

    // static detection function
    static bool detect(AP_SerialManager &serial_manager);

    // update state
    void update(void);

    // get maximum and minimum distances (in meters) of sensor
    float distance_max() const;
    float distance_min() const;

private:
    enum rp_state {
            rp_unknown = 0,
            rp_resetted,
            rp_responding,
            rp_measurements,
            rp_health
        };

    enum ResponseType {
        ResponseType_Descriptor = 0,
        ResponseType_SCAN,
        ResponseType_EXPRESS,
        ResponseType_Health
    };

    // initialise sensor (returns true if sensor is successfully initialised)
    bool initialise();
    void init_sectors();
    void set_scan_mode();

    // send request for something from sensor
    void send_request_for_health();
    void parse_response_data();
    void parse_response_descriptor();
    void get_readings();
    void reset_rplidar();

    // reply related variables
    AP_HAL::UARTDriver *_uart;
    uint8_t _descriptor[7];
    char _rp_systeminfo[63];
    bool _descriptor_data;
    bool _information_data;
    bool _resetted;
    bool _initialised;
    bool _sector_initialised;

    uint8_t _payload_length;
    uint8_t _cnt;
    uint8_t _sync_error ;
    uint16_t _byte_count;

    //新增 改动
	float angle_left_front;
	float distant_left_front;
	uint8_t point_front_find=0;
    float angle_right_front;
    float distant_right_front;
    float L_left_front, L_right_front;

    float angle_left_back;
    float distant_left_back;
    uint8_t point_back_find=0;
    float angle_right_back;
    float distant_right_back;
    float L_left_back, L_right_back;


    //float distance_object_front,avoid_direction_front,avoid_distance_front;
    //float distance_object_back,avoid_direction_back,avoid_distance_back;





    // request related variables
    enum ResponseType _response_type;         ///< response from the lidar
    enum rp_state _rp_state;
    uint8_t   _last_sector;                   ///< last sector requested
    uint32_t  _last_request_ms;               ///< system time of last request
    uint32_t  _last_distance_received_ms;     ///< system time of last distance measurement received from sensor
    uint32_t  _last_reset_ms;

    // sector related variables
    float _angle_deg_last;
    float _distance_m_last;

    struct PACKED _sensor_scan {
        uint8_t startbit      : 1;            ///< on the first revolution 1 else 0
        uint8_t not_startbit  : 1;            ///< complementary to startbit
        uint8_t quality       : 6;            ///< Related the reflected laser pulse strength
        uint8_t checkbit      : 1;            ///< always set to 1
        uint16_t angle_q6     : 15;           ///< Actual heading = angle_q6/64.0 Degree
        uint16_t distance_q2  : 16;           ///< Actual Distance = distance_q2/4.0 mm
    };

    struct PACKED _sensor_health {
        uint8_t status;                       ///< status definition: 0 good, 1 warning, 2 error
        uint16_t error_code;                  ///< the related error code
    };

    union PACKED {
        DEFINE_BYTE_ARRAY_METHODS
        _sensor_scan sensor_scan;
        _sensor_health sensor_health;
    } payload;
};
