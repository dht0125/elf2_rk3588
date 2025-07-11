// control.c
#include "control.h"
#include <stdio.h>
#include <stdlib.h>

// 全局变量定义
volatile float traffic_density = 0.0;    // 车流量密度
volatile int illegal_vehicles = 0;       // 违章车辆数目
volatile int green_extension = 0;        // 绿灯延长时间(秒)
volatile int system_running = 1;         // 系统运行标志
sem_t data_mutex;                        // 数据保护信号量
static int simulation_mode = 0;          // 模拟模式标志

int init_traffic_light(TrafficLight* light) {
    printf("正在初始化交通灯GPIO控制...\n");
    printf("尝试打开GPIO芯片: %s\n", CHIP_NAME);
    
    light->chip = gpiod_chip_open_by_name(CHIP_NAME);
    if (!light->chip) {
        fprintf(stderr, "无法打开GPIO芯片 %s\n", CHIP_NAME);
        fprintf(stderr, "请检查:\n");
        fprintf(stderr, "1. GPIO芯片是否存在: ls /dev/gpiochip*\n");
        fprintf(stderr, "2. 是否有GPIO库: sudo apt install libgpiod-dev\n");
        fprintf(stderr, "3. 用户权限: sudo usermod -a -G gpio $USER\n");
        fprintf(stderr, "切换到模拟模式...\n");
        simulation_mode = 1;
        return 0;  // 模拟模式返回成功
    }
    
    printf("GPIO芯片 %s 打开成功\n", CHIP_NAME);

    printf("获取GPIO线: 红灯=%d, 黄灯=%d, 绿灯=%d\n", RED_LINE, YELLOW_LINE, GREEN_LINE);
    
    light->red = gpiod_chip_get_line(light->chip, RED_LINE);
    light->yellow = gpiod_chip_get_line(light->chip, YELLOW_LINE);
    light->green = gpiod_chip_get_line(light->chip, GREEN_LINE);

    if (!light->red || !light->yellow || !light->green) {
        fprintf(stderr, "获取GPIO线失败\n");
        if (!light->red) fprintf(stderr, "- 红灯GPIO线 %d 获取失败\n", RED_LINE);
        if (!light->yellow) fprintf(stderr, "- 黄灯GPIO线 %d 获取失败\n", YELLOW_LINE);
        if (!light->green) fprintf(stderr, "- 绿灯GPIO线 %d 获取失败\n", GREEN_LINE);
        gpiod_chip_close(light->chip);
        return 1;
    }
    
    printf("GPIO线获取成功\n");

    printf("配置GPIO线为输出模式...\n");
    
    if (gpiod_line_request_output(light->red, CONSUMER, 0) < 0) {
        fprintf(stderr, "红灯GPIO线 %d 配置输出失败\n", RED_LINE);
        gpiod_chip_close(light->chip);
        return 1;
    }
    
    if (gpiod_line_request_output(light->yellow, CONSUMER, 0) < 0) {
        fprintf(stderr, "黄灯GPIO线 %d 配置输出失败\n", YELLOW_LINE);
        gpiod_chip_close(light->chip);
        return 1;
    }
    
    if (gpiod_line_request_output(light->green, CONSUMER, 0) < 0) {
        fprintf(stderr, "绿灯GPIO线 %d 配置输出失败\n", GREEN_LINE);
        gpiod_chip_close(light->chip);
        return 1;
    }

    printf("交通灯GPIO初始化完成!\n");
    return 0;
}

void set_light(TrafficLight* light, int red_on, int yellow_on, int green_on) {
    if (simulation_mode) {
        printf("🚦 [模拟模式] 交通灯状态: 红=%s, 黄=%s, 绿=%s\n", 
               red_on ? "亮" : "灭", 
               yellow_on ? "亮" : "灭", 
               green_on ? "亮" : "灭");
        return;
    }
    
    printf("设置交通灯: 红=%d, 黄=%d, 绿=%d\n", red_on, yellow_on, green_on);
    
    if (gpiod_line_set_value(light->red, red_on) < 0) {
        fprintf(stderr, "设置红灯GPIO失败\n");
    }
    if (gpiod_line_set_value(light->yellow, yellow_on) < 0) {
        fprintf(stderr, "设置黄灯GPIO失败\n");
    }
    if (gpiod_line_set_value(light->green, green_on) < 0) {
        fprintf(stderr, "设置绿灯GPIO失败\n");
    }
}

void close_traffic_light(TrafficLight* light) {
    if (simulation_mode) {
        printf("🚦 [模拟模式] 交通灯系统已关闭\n");
        return;
    }
    
    if (light->chip) {
        gpiod_chip_close(light->chip);
        printf("交通灯GPIO已关闭\n");
    }
}

void read_traffic_data_from_shared_memory() {
    int fd = shm_open(SHARED_MEM_NAME, O_RDONLY, 0666);
    if (fd == -1) {
        perror("shm_open");
        return;
    }

    char* shared_memory = mmap(NULL, SHARED_MEM_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    if (shared_memory == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return;
    }

    // 使用cJSON解析共享内存中的JSON数据
    cJSON *json = cJSON_Parse(shared_memory);
    if (json != NULL) {
        cJSON *density_obj = cJSON_GetObjectItem(json, "density");
        cJSON *illegal_vehicles_obj = cJSON_GetObjectItem(json, "illegal_vehicles");
        if ((density_obj && density_obj->type == cJSON_Number) && (illegal_vehicles_obj && illegal_vehicles_obj->type == cJSON_Number)) {
            traffic_density = density_obj->valuedouble;
            illegal_vehicles = illegal_vehicles_obj->valueint;
        }
        cJSON_Delete(json);
    }

    munmap(shared_memory, SHARED_MEM_SIZE);
    close(fd);
}