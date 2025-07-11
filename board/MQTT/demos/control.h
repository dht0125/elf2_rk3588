// control.h
#ifndef CONTROL_H
#define CONTROL_H

#include <gpiod.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>
#include "cJSON.h"

#define CONSUMER "traffic-light"
#define RED_LINE    5
#define YELLOW_LINE 6
#define GREEN_LINE  2
#define CHIP_NAME   "gpiochip3"

typedef struct gpiod_chip gpiod_chip;
typedef struct gpiod_line gpiod_line;


#define NORMAL_GREEN_TIME 25  // 正常绿灯时间(秒)
#define DENSITY_THRESHOLD_A 0.5  // 密度阈值A
#define DENSITY_THRESHOLD_B 1.0  // 密度阈值B
#define EXTENSION_SHORT 10      // 轻度拥堵延长时间(秒)
#define EXTENSION_LONG 15       // 严重拥堵延长时间(秒)

#define SHARED_MEM_NAME "traffic_data_shm"  // 共享内存名称
#define SHARED_MEM_SIZE 4096  // 共享内存大小（足够存储JSON数据）

// 全局变量声明
extern volatile float traffic_density;    // 车流量密度
extern volatile int illegal_vehicles;     // 违章车辆数目
extern volatile int green_extension;      // 绿灯延长时间(秒)
extern volatile int system_running;       // 系统运行标志
extern sem_t data_mutex;                  // 数据保护信号量

typedef struct {
    gpiod_chip* chip;
    gpiod_line* red;
    gpiod_line* yellow;
    gpiod_line* green;
} TrafficLight;

int init_traffic_light(TrafficLight* light);
void set_light(TrafficLight* light, int red_on, int yellow_on, int green_on);
void close_traffic_light(TrafficLight* light);
void read_traffic_data_from_shared_memory();

#endif