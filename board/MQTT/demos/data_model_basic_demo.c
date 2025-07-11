/*
 * 这个例程适用于`Linux`这类支持pthread的POSIX设备, 它演示了用SDK配置MQTT参数并建立连接, 之后创建2个线程
 *
 * + 一个线程用于保活长连接
 * + 一个线程用于接收消息, 并在有消息到达时进入默认的数据回调, 在连接状态变化时进入事件回调
 *
 * 接着演示了在MQTT连接上进行属性上报, 事件上报, 以及处理收到的属性设置, 服务调用, 取消这些代码段落的注释即可观察运行效果
 *
 * 需要用户关注或修改的部分, 已经用 TODO 在注释中标明
 *
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <semaphore.h>

#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include "sys/ioctl.h"
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "control.h"
#include "aiot_state_api.h"
#include "aiot_sysdep_api.h"
#include "aiot_mqtt_api.h"
#include "aiot_dm_api.h"
#include "cJSON.h"

//adc
#define voltage5_raw "/sys/bus/iio/devices/iio:device0/in_voltage5_raw"
#define voltage_scale "/sys/bus/iio/devices/iio:device0/in_voltage_scale"
//aht20
#define AHT20_DEV "/dev/aht20"
//led
#define LED1_BRIGHTNESS "/sys/class/leds/led1/brightness"
#define LED2_BRIGHTNESS "/sys/class/leds/led2/brightness"


/* TODO: 替换为自己设备的三元组 */
const char *product_key       = "k260tC1CBFv";
const char *device_name       = "control_unit_1";
const char *device_secret     = "deee543e91b1642b36b197863ee5da5a";

/* 打印设备信息用于调试 */


/* 打印物模型属性说明 */
void print_property_model_guide() {
    printf("\n🔧 物模型属性配置指南:\n");
    printf("请在阿里云物联网平台为设备添加以下属性:\n");
    printf("┌─────────────────┬──────────────┬──────────────────┐\n");
    printf("│ 属性标识符      │ 数据类型     │ 说明             │\n");
    printf("├─────────────────┼──────────────┼──────────────────┤\n");
    printf("│ TrafficDensity  │ float/double │ 交通密度         │\n");
    printf("│ IllegalVehicles │ int32        │ 违章车辆         │\n");
    printf("│ GreenExtension  │ int32        │ 绿灯延长         │\n");
    printf("│ RedLED          │ int32        │ 红灯状态         │\n");
    printf("│ YellowLED       │ int32        │ 黄灯状态         │\n");
    printf("│ GreenLED        │ int32        │ 绿灯状态         │\n");
    printf("│ VideoStreamURL  │ string       │ 视频流地址       │\n");
    printf("│ CurrentCarCount │ int32        │ 当前车辆数       │\n");
    printf("│ TotalDetections │ int32        │ 总检测次数       │\n");
    printf("│ ProcessingFPS   │ float        │ 处理帧率         │\n");
    printf("│ AIProcessTime   │ float        │ AI处理时间(ms)   │\n");
    printf("│ SystemStatus    │ string       │ 系统状态         │\n");
    printf("└─────────────────┴──────────────┴──────────────────┘\n");
    printf("配置路径: 设备管理 > 产品 > 功能定义 > 编辑草稿 > 添加功能\n\n");
    
    printf("❗ 常见问题排查:\n");
    printf("1. 确保物模型属性标识符与代码中完全一致(区分大小写)\n");
    printf("2. 检查数据类型匹配(int32 vs float vs double)\n");
    printf("3. 发布物模型: 编辑草稿后要点击'发布上线'\n");
    printf("4. 设备激活: 确保设备在线且已激活\n");
    printf("5. 查看数据: 设备管理 > 设备 > 运行状态 > 属性\n\n");
}

/*
    TODO: 替换为自己实例的接入点

    对于企业实例, 或者2021年07月30日之后（含当日）开通的物联网平台服务下公共实例
    mqtt_host的格式为"${YourInstanceId}.mqtt.iothub.aliyuncs.com"
    其中${YourInstanceId}: 请替换为您企业/公共实例的Id

    对于2021年07月30日之前（不含当日）开通的物联网平台服务下公共实例，请使用旧版接入点。
    详情请见: https://help.aliyun.com/document_detail/147356.html
*/
const char  *mqtt_host = "iot-06z00ae5m2066i3.mqtt.iothub.aliyuncs.com";
/* 
    原端口：1883/443，对应的证书(GlobalSign R1),于2028年1月过期，届时可能会导致设备不能建连。
    (推荐)新端口：8883，将搭载新证书，由阿里云物联网平台自签证书，于2053年7月过期。
*/
const uint16_t port = 1883;

void print_device_info() {
    printf("=== 设备信息 ===\n");
    printf("ProductKey: %s\n", product_key);
    printf("DeviceName: %s\n", device_name);
    printf("DeviceSecret: %s\n", device_secret);
    printf("MQTT Host: %s\n", mqtt_host);
    printf("================\n");
}
/* 位于portfiles/aiot_port文件夹下的系统适配函数集合 */
extern aiot_sysdep_portfile_t g_aiot_sysdep_portfile;

/* 位于external/ali_ca_cert.c中的服务器证书 */
extern const char *ali_ca_cert;

static pthread_t g_mqtt_process_thread;
static pthread_t g_mqtt_recv_thread;
static uint8_t g_mqtt_process_thread_running = 0;
static uint8_t g_mqtt_recv_thread_running = 0;

// 函数声明
int demo_send_property_post(void *dm_handle, const char *params);
int demo_send_property_post_enhanced(void *dm_handle, const char *params);
int demo_send_device_status(void *dm_handle);
int demo_send_heartbeat(void *dm_handle);
int fetch_video_data_from_flask(char *buffer, size_t buffer_size);
int demo_send_video_data(void *dm_handle);

// 视频数据结构
typedef struct {
    float traffic_density;
    int car_count;
    int total_detections;
    float processing_fps;
    float ai_process_time;
    char system_status[32];
    time_t last_update;
} VideoData;

// 全局视频数据
VideoData video_data = {0.0, 0, 0, 0.0, 0.0, "offline", 0};

//function
float get_adc(void)
{
	int raw_fd, scale_fd;
	char buff[20];
	int raw;
	double scale;

	/* 1.打开文件 */
	raw_fd = open(voltage5_raw, O_RDONLY);
	if(raw_fd < 0){
		printf("open raw_fd failed!\n");
		return -1;
	}
	scale_fd = open(voltage_scale, O_RDONLY);
	if(scale_fd < 0){
		printf("open scale_fd failed!\n");
		return -1;
	}

	/* 2.读取文件 */
	// rewind(raw_fd);   // 将光标移回文件开头
	if (read(raw_fd, buff, sizeof(buff)) < 0) {
		perror("read raw_fd");
		close(raw_fd);
		close(scale_fd);
		return -1;
	}
	raw = atoi(buff);
	memset(buff, 0, sizeof(buff));
	// rewind(scale_fd);   // 将光标移回文件开头
	if (read(scale_fd, buff, sizeof(buff)) < 0) {
		perror("read scale_fd");
		close(raw_fd);
		close(scale_fd);
		return -1;
	}
	scale = atof(buff);
	printf("ADC原始值：%d，电压值：%.3fV\r\n", raw, raw * scale / 1000.f);
	close(raw_fd);
	close(scale_fd);
	return raw * scale / 1000.f;
}

int get_aht20(float* ath20_data)
{
	int fd;
	unsigned int databuf[2];
	int c1,t1; 
	float hum,temp;
	int ret = 0;
 
	fd = open(AHT20_DEV, O_RDWR);
	if(fd < 0) {
		printf("can't open file %s\r\n", AHT20_DEV);
		return -1;
	}
 
	ret = read(fd, databuf, sizeof(databuf));
	if(ret == 0) { 			/* ?????? */
	    c1 = databuf[0]*1000/1024/1024;  //
	    t1 = databuf[1] *200*10/1024/1024-500;
	    hum = (float)c1/10.0;
	    temp = (float)t1/10.0;

	    printf("hum = %0.2f temp = %0.2f \r\n",hum,temp);
        *ath20_data = hum;
        *(ath20_data+1) = temp;
	}

	close(fd);
    return 0;
}

int get_led(int led_sel)
{
    int led;
    char buff[20];
    int state=0;
    if(led_sel == 2)
    {
        led=open(LED2_BRIGHTNESS, O_RDWR);
    }else{
        led=open(LED1_BRIGHTNESS, O_RDWR);
    }
    if(led<0)
    {
        perror("open device led error");
        exit(1);
    }

	if (read(led, buff, sizeof(buff)) < 0) {
		perror("read led");
		close(led);
		return -1;
	}
	state = atoi(buff);

    close(led);
    return state;
}

void set_led(int led_sel, char state)
{
    int led;
    if(led_sel == 2)
    {
        led=open(LED2_BRIGHTNESS, O_RDWR);
    }else{
        led=open(LED1_BRIGHTNESS, O_RDWR);
    }
    if(led<0)
    {
        perror("open device led error");
        exit(1);
    }

	if (write(led, &state, 1) < 0) {
		perror("write led");
	}
    close(led);
}



/* TODO: 如果要关闭日志, 就把这个函数实现为空, 如果要减少日志, 可根据code选择不打印
 *
 * 上面这条日志的code就是0317(十六进制), code值的定义见core/aiot_state_api.h
 *
 */

/* 日志回调函数, SDK的日志会从这里输出 */
int32_t demo_state_logcb(int32_t code, char *message)
{
    printf("%s", message);
    return 0;
}

/* MQTT事件回调函数, 当网络连接/重连/断开时被触发, 事件定义见core/aiot_mqtt_api.h */
void demo_mqtt_event_handler(void *handle, const aiot_mqtt_event_t *event, void *userdata)
{
    switch (event->type) {
        /* SDK因为用户调用了aiot_mqtt_connect()接口, 与mqtt服务器建立连接已成功 */
        case AIOT_MQTTEVT_CONNECT: {
            printf("AIOT_MQTTEVT_CONNECT\n");
        }
        break;

        /* SDK因为网络状况被动断连后, 自动发起重连已成功 */
        case AIOT_MQTTEVT_RECONNECT: {
            printf("AIOT_MQTTEVT_RECONNECT\n");
        }
        break;

        /* SDK因为网络的状况而被动断开了连接, network是底层读写失败, heartbeat是没有按预期得到服务端心跳应答 */
        case AIOT_MQTTEVT_DISCONNECT: {
            char *cause = (event->data.disconnect == AIOT_MQTTDISCONNEVT_NETWORK_DISCONNECT) ? ("network disconnect") :
                          ("heartbeat disconnect");
            printf("AIOT_MQTTEVT_DISCONNECT: %s\n", cause);
        }
        break;

        default: {

        }
    }
}

/* 执行aiot_mqtt_process的线程, 包含心跳发送和QoS1消息重发 */
void *demo_mqtt_process_thread(void *args)
{
    int32_t res = STATE_SUCCESS;

    while (g_mqtt_process_thread_running) {
        res = aiot_mqtt_process(args);
        if (res == STATE_USER_INPUT_EXEC_DISABLED) {
            break;
        }
        sleep(1);
    }
    return NULL;
}

/* 执行aiot_mqtt_recv的线程, 包含网络自动重连和从服务器收取MQTT消息 */
void *demo_mqtt_recv_thread(void *args)
{
    int32_t res = STATE_SUCCESS;

    while (g_mqtt_recv_thread_running) {
        res = aiot_mqtt_recv(args);
        if (res < STATE_SUCCESS) {
            if (res == STATE_USER_INPUT_EXEC_DISABLED) {
                break;
            }
            sleep(1);
        }
    }
    return NULL;
}

static void demo_dm_recv_generic_reply(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    printf("demo_dm_recv_generic_reply msg_id = %d, code = %d, data = %.*s, message = %.*s\r\n",
           recv->data.generic_reply.msg_id,
           recv->data.generic_reply.code,
           recv->data.generic_reply.data_len,
           recv->data.generic_reply.data,
           recv->data.generic_reply.message_len,
           recv->data.generic_reply.message);
}

//自己写

void* video_data_collection(void* arg) {
    void* aliyun_handle = (void*)arg;
    char data_str[128];
    int ret;
    
    printf("启动视频流数据采集线程...\n");
    
    // 初始化随机数种子
    srand(time(NULL));
    
    while (system_running) {
        // 从共享内存读取交通数据 (暂时注释掉，避免访问不存在的共享内存)
        // read_traffic_data_from_shared_memory();

        // 保护共享数据
        sem_wait(&data_mutex);
        
        // 模拟交通数据 (替代共享内存读取)
        traffic_density = 0.3 + (rand() % 100) / 100.0;  // 0.3-1.3 之间的随机值
        illegal_vehicles = rand() % 5;  // 0-4 之间的随机值
        
        // 计算绿灯延长时间
        if (traffic_density > DENSITY_THRESHOLD_A && traffic_density <= DENSITY_THRESHOLD_B) {
            green_extension = EXTENSION_SHORT;
        } else if (traffic_density > DENSITY_THRESHOLD_B) {
            green_extension = EXTENSION_LONG;
        } else {
            green_extension = 0;
        }
        
        // 释放信号量
        sem_post(&data_mutex);
        
        // 构建JSON数据并发送到阿里云
        memset(data_str, 0, sizeof(data_str));
        sprintf(data_str, 
            "{\"TrafficDensity\":%.2f,\"IllegalVehicles\":%d,\"GreenExtension\":%d}",
            traffic_density, illegal_vehicles, green_extension);
        
        ret = demo_send_property_post(aliyun_handle, data_str);
        if (ret < 0) {
            printf("发送数据到阿里云失败，错误码: %d\n", ret);
        } else {
            printf("数据已发送: 密度=%.2f, 违章车辆=%d, 绿灯延长=%d秒 (消息ID: %d)\n", 
                  traffic_density, illegal_vehicles, green_extension, ret);
        }
        
        // 每5秒采集一次数据
        sleep(5);
    }
    
    printf("视频流数据采集线程已停止\n");
    return NULL;
}




static void demo_dm_recv_property_set(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    int led;
    char state=0;
    printf("demo_dm_recv_property_set msg_id = %ld, params = %.*s\r\n",
           (unsigned long)recv->data.property_set.msg_id,
           recv->data.property_set.params_len,
           recv->data.property_set.params);

    /* TODO: 以下代码演示如何对来自云平台的属性设置指令进行应答, 用户可取消注释查看演示效果 */
    cJSON* cjson_result = NULL;
    cJSON* cjson_set1 = NULL;
    cJSON* cjson_set2 = NULL;

    cjson_result = cJSON_Parse(recv->data.property_set.params);
    if(cjson_result == NULL)
    {
        printf("parse fail.\n");
        return;
    }
    //{"LEDSwitch":0}
	cjson_set1 = cJSON_GetObjectItem(cjson_result,"LEDSwitch");
    if(cjson_set1)
    {
        printf("LED1 set %d\n",cjson_set1->valueint);
        state = cjson_set1->valueint+48;
        
        led=open(LED1_BRIGHTNESS, O_WRONLY);//led是文件操作符，只读
        if(led<0)
        {
            perror("open device led1");
            exit(1);
        }
        if (write(led, &state, 1) < 0) {
            perror("write led1");
        }
        close(led);
    }
    
    cjson_set2 = cJSON_GetObjectItem(cjson_result,"LEDSwitch2");
    if(cjson_set2){
        printf("LED2 set %d\n",cjson_set2->valueint);
        state = cjson_set2->valueint+48;

        led=open(LED2_BRIGHTNESS, O_WRONLY);//led是文件操作符，只读
        if(led<0)
        {
            perror("open device led1");
            exit(1);
        }
        if (write(led, &state, 1) < 0) {
            perror("write led2");
        }
        close(led);   
    }
	
	//释放内存
	cJSON_Delete(cjson_result);

    {
        aiot_dm_msg_t msg;

        memset(&msg, 0, sizeof(aiot_dm_msg_t));
        msg.type = AIOT_DMMSG_PROPERTY_SET_REPLY;
        msg.data.property_set_reply.msg_id = recv->data.property_set.msg_id;
        msg.data.property_set_reply.code = 200;
        msg.data.property_set_reply.data = "{}";
        int32_t res = aiot_dm_send(dm_handle, &msg);
        if (res < 0) {
            printf("aiot_dm_send failed\r\n");
        }
    }
    
}

//自己写

static void demo_dm_recv_async_service_invoke(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    printf("demo_dm_recv_async_service_invoke msg_id = %ld, service_id = %s, params = %.*s\r\n",
           (unsigned long)recv->data.async_service_invoke.msg_id,
           recv->data.async_service_invoke.service_id,
           recv->data.async_service_invoke.params_len,
           recv->data.async_service_invoke.params);

    /* TODO: 以下代码演示如何对来自云平台的异步服务调用进行应答, 用户可取消注释查看演示效果
        *
        * 注意: 如果用户在回调函数外进行应答, 需要自行保存msg_id, 因为回调函数入参在退出回调函数后将被SDK销毁, 不可以再访问到
        */

    /*
    {
        aiot_dm_msg_t msg;

        memset(&msg, 0, sizeof(aiot_dm_msg_t));
        msg.type = AIOT_DMMSG_ASYNC_SERVICE_REPLY;
        msg.data.async_service_reply.msg_id = recv->data.async_service_invoke.msg_id;
        msg.data.async_service_reply.code = 200;
        msg.data.async_service_reply.service_id = "ToggleLightSwitch";
        msg.data.async_service_reply.data = "{\"dataA\": 20}";
        int32_t res = aiot_dm_send(dm_handle, &msg);
        if (res < 0) {
            printf("aiot_dm_send failed\r\n");
        }
    }
    */
}

static void demo_dm_recv_sync_service_invoke(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    printf("demo_dm_recv_sync_service_invoke msg_id = %ld, rrpc_id = %s, service_id = %s, params = %.*s\r\n",
           (unsigned long)recv->data.sync_service_invoke.msg_id,
           recv->data.sync_service_invoke.rrpc_id,
           recv->data.sync_service_invoke.service_id,
           recv->data.sync_service_invoke.params_len,
           recv->data.sync_service_invoke.params);

    /* TODO: 以下代码演示如何对来自云平台的同步服务调用进行应答, 用户可取消注释查看演示效果
        *
        * 注意: 如果用户在回调函数外进行应答, 需要自行保存msg_id和rrpc_id字符串, 因为回调函数入参在退出回调函数后将被SDK销毁, 不可以再访问到
        */

    /*
    {
        aiot_dm_msg_t msg;

        memset(&msg, 0, sizeof(aiot_dm_msg_t));
        msg.type = AIOT_DMMSG_SYNC_SERVICE_REPLY;
        msg.data.sync_service_reply.rrpc_id = recv->data.sync_service_invoke.rrpc_id;
        msg.data.sync_service_reply.msg_id = recv->data.sync_service_invoke.msg_id;
        msg.data.sync_service_reply.code = 200;
        msg.data.sync_service_reply.service_id = "SetLightSwitchTimer";
        msg.data.sync_service_reply.data = "{}";
        int32_t res = aiot_dm_send(dm_handle, &msg);
        if (res < 0) {
            printf("aiot_dm_send failed\r\n");
        }
    }
    */
}

static void demo_dm_recv_raw_data(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    printf("demo_dm_recv_raw_data raw data len = %d\r\n", recv->data.raw_data.data_len);
    /* TODO: 以下代码演示如何发送二进制格式数据, 若使用需要有相应的数据透传脚本部署在云端 */
    /*
    {
        aiot_dm_msg_t msg;
        uint8_t raw_data[] = {0x01, 0x02};

        memset(&msg, 0, sizeof(aiot_dm_msg_t));
        msg.type = AIOT_DMMSG_RAW_DATA;
        msg.data.raw_data.data = raw_data;
        msg.data.raw_data.data_len = sizeof(raw_data);
        aiot_dm_send(dm_handle, &msg);
    }
    */
}

static void demo_dm_recv_raw_sync_service_invoke(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    printf("demo_dm_recv_raw_sync_service_invoke raw sync service rrpc_id = %s, data_len = %d\r\n",
           recv->data.raw_service_invoke.rrpc_id,
           recv->data.raw_service_invoke.data_len);
}

static void demo_dm_recv_raw_data_reply(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    printf("demo_dm_recv_raw_data_reply receive reply for up_raw msg, data len = %d\r\n", recv->data.raw_data.data_len);
    /* TODO: 用户处理下行的二进制数据, 位于recv->data.raw_data.data中 */
}

/* 用户数据接收处理回调函数 */
static void demo_dm_recv_handler(void *dm_handle, const aiot_dm_recv_t *recv, void *userdata)
{
    printf("demo_dm_recv_handler, type = %d\r\n", recv->type);

    switch (recv->type) {

        /* 属性上报, 事件上报, 获取期望属性值或者删除期望属性值的应答 */
        case AIOT_DMRECV_GENERIC_REPLY: {
            demo_dm_recv_generic_reply(dm_handle, recv, userdata);
        }
        break;

        /* 属性设置 */
        case AIOT_DMRECV_PROPERTY_SET: {
            demo_dm_recv_property_set(dm_handle, recv, userdata);
        }
        break;

        /* 异步服务调用 */
        case AIOT_DMRECV_ASYNC_SERVICE_INVOKE: {
            demo_dm_recv_async_service_invoke(dm_handle, recv, userdata);
        }
        break;

        /* 同步服务调用 */
        case AIOT_DMRECV_SYNC_SERVICE_INVOKE: {
            demo_dm_recv_sync_service_invoke(dm_handle, recv, userdata);
        }
        break;

        /* 下行二进制数据 */
        case AIOT_DMRECV_RAW_DATA: {
            demo_dm_recv_raw_data(dm_handle, recv, userdata);
        }
        break;

        /* 二进制格式的同步服务调用, 比单纯的二进制数据消息多了个rrpc_id */
        case AIOT_DMRECV_RAW_SYNC_SERVICE_INVOKE: {
            demo_dm_recv_raw_sync_service_invoke(dm_handle, recv, userdata);
        }
        break;

        /* 上行二进制数据后, 云端的回复报文 */
        case AIOT_DMRECV_RAW_DATA_REPLY: {
            demo_dm_recv_raw_data_reply(dm_handle, recv, userdata);
        }
        break;

        default:
            break;
    }
}

/* 属性上报函数演示 */
int demo_send_property_post(void *dm_handle, const char *params)
{
    aiot_dm_msg_t msg;
    int result;

    printf("📤 [属性上报] 发送数据: %s\n", params);
    
    memset(&msg, 0, sizeof(aiot_dm_msg_t));
    msg.type = AIOT_DMMSG_PROPERTY_POST;
    msg.data.property_post.params = (char *)params;

    result = aiot_dm_send(dm_handle, &msg);
    
    if (result < 0) {
        printf("❌ [属性上报] 发送失败，错误码: %d\n", result);
    } else {
        printf("✅ [属性上报] 发送成功，消息ID: %d\n", result);
    }
    
    return result;
}

/* 增强的属性上报函数，包含时间戳和版本信息 */
int demo_send_property_post_enhanced(void *dm_handle, const char *params)
{
    char enhanced_params[512];
    time_t current_time = time(NULL);
    
    // 添加时间戳到属性数据中
    if (strstr(params, "timestamp") == NULL) {
        // 移除最后的 '}' 并添加时间戳
        int len = strlen(params);
        if (len > 0 && params[len-1] == '}') {
            snprintf(enhanced_params, sizeof(enhanced_params), 
                    "%.*s,\"timestamp\":%ld}", len-1, params, current_time);
        } else {
            snprintf(enhanced_params, sizeof(enhanced_params), "%s", params);
        }
    } else {
        snprintf(enhanced_params, sizeof(enhanced_params), "%s", params);
    }
    
    return demo_send_property_post(dm_handle, enhanced_params);
}

/* 发送设备状态信息 */
int demo_send_device_status(void *dm_handle)
{
    char status_data[256];
    time_t current_time = time(NULL);
    
    snprintf(status_data, sizeof(status_data), 
            "{\"deviceStatus\":\"online\",\"systemRunning\":%d,\"timestamp\":%ld}",
            system_running, current_time);
    
    printf("📊 [设备状态] 发送状态信息\n");
    return demo_send_property_post(dm_handle, status_data);
}

/* 发送心跳信号 */
int demo_send_heartbeat(void *dm_handle)
{
    char heartbeat_data[128];
    time_t current_time = time(NULL);
    
    snprintf(heartbeat_data, sizeof(heartbeat_data), 
            "{\"heartbeat\":%ld}", current_time);
    
    printf("💓 [心跳] 发送心跳信号\n");
    return demo_send_event_post(dm_handle, "Heartbeat", heartbeat_data);
}

int32_t demo_send_property_batch_post(void *dm_handle, char *params)
{
    aiot_dm_msg_t msg;

    memset(&msg, 0, sizeof(aiot_dm_msg_t));
    msg.type = AIOT_DMMSG_PROPERTY_BATCH_POST;
    msg.data.property_post.params = params;

    return aiot_dm_send(dm_handle, &msg);
}

/* 事件上报函数演示 */
int32_t demo_send_event_post(void *dm_handle, char *event_id, char *params)
{
    aiot_dm_msg_t msg;

    memset(&msg, 0, sizeof(aiot_dm_msg_t));
    msg.type = AIOT_DMMSG_EVENT_POST;
    msg.data.event_post.event_id = event_id;
    msg.data.event_post.params = params;

    return aiot_dm_send(dm_handle, &msg);
}

/* 演示了获取属性LightSwitch的期望值, 用户可将此函数加入到main函数中运行演示 */
int32_t demo_send_get_desred_requset(void *dm_handle)
{
    aiot_dm_msg_t msg;

    memset(&msg, 0, sizeof(aiot_dm_msg_t));
    msg.type = AIOT_DMMSG_GET_DESIRED;
    msg.data.get_desired.params = "[\"LightSwitch\"]";

    return aiot_dm_send(dm_handle, &msg);
}

/* 演示了删除属性LightSwitch的期望值, 用户可将此函数加入到main函数中运行演示 */
int32_t demo_send_delete_desred_requset(void *dm_handle)
{
    aiot_dm_msg_t msg;

    memset(&msg, 0, sizeof(aiot_dm_msg_t));
    msg.type = AIOT_DMMSG_DELETE_DESIRED;
    msg.data.get_desired.params = "{\"LightSwitch\":{}}";

    return aiot_dm_send(dm_handle, &msg);
}

/* 从Flask服务器获取视频数据 */
int fetch_video_data_from_flask(char *buffer, size_t buffer_size) {
    FILE *fp;
    char cmd[256];
    int ret = 0;
    
    // 使用curl从Flask服务器获取数据
    snprintf(cmd, sizeof(cmd), "curl -s -X GET http://localhost:5000/get_detection_data 2>/dev/null");
    
    fp = popen(cmd, "r");
    if (fp == NULL) {
        printf("❌ 无法执行curl命令\n");
        return -1;
    }
    
    // 读取响应数据
    if (fgets(buffer, buffer_size, fp) != NULL) {
        ret = strlen(buffer);
        // 移除换行符
        if (buffer[ret-1] == '\n') {
            buffer[ret-1] = '\0';
            ret--;
        }
    } else {
        printf("❌ 无法读取Flask服务器响应\n");
        ret = -1;
    }
    
    pclose(fp);
    return ret;
}

/* 解析视频数据并更新全局变量 */
int parse_video_data(const char *json_data) {
    cJSON *json = cJSON_Parse(json_data);
    if (json == NULL) {
        printf("❌ 解析JSON数据失败\n");
        return -1;
    }
    
    cJSON *item;
    
    // 解析各个字段
    item = cJSON_GetObjectItem(json, "car_count");
    if (item && item->type == cJSON_Number) {
        video_data.car_count = item->valueint;
    }
    
    item = cJSON_GetObjectItem(json, "total_detections");
    if (item && item->type == cJSON_Number) {
        video_data.total_detections = item->valueint;
    }
    
    item = cJSON_GetObjectItem(json, "traffic_density");
    if (item && item->type == cJSON_Number) {
        video_data.traffic_density = (float)item->valuedouble;
    }
    
    item = cJSON_GetObjectItem(json, "frame_fps");
    if (item && item->type == cJSON_Number) {
        video_data.processing_fps = (float)item->valuedouble;
    }
    
    item = cJSON_GetObjectItem(json, "ai_process_time");
    if (item && item->type == cJSON_Number) {
        video_data.ai_process_time = (float)item->valuedouble;
    }
    
    item = cJSON_GetObjectItem(json, "timestamp");
    if (item && item->type == cJSON_Number) {
        time_t current_time = time(NULL);
        time_t data_time = (time_t)item->valuedouble;
        
        // 检查数据是否是最近的（10秒内）
        if (current_time - data_time < 10) {
            strcpy(video_data.system_status, "online");
        } else {
            strcpy(video_data.system_status, "offline");
        }
        
        video_data.last_update = data_time;
    }
    
    cJSON_Delete(json);
    
    printf("📊 视频数据更新: 车辆=%d, 总检测=%d, 密度=%.2f, FPS=%.1f, 状态=%s\n",
           video_data.car_count, video_data.total_detections, video_data.traffic_density,
           video_data.processing_fps, video_data.system_status);
    
    return 0;
}

/* 发送视频数据到阿里云 */
int demo_send_video_data(void *dm_handle) {
    char data_str[512];
    char video_stream_url[128];
    
    // 构建视频流URL
    snprintf(video_stream_url, sizeof(video_stream_url), "http://localhost:5000/video_stream");
    
    // 构建完整的属性数据
    snprintf(data_str, sizeof(data_str),
        "{"
        "\"TrafficDensity\":%.2f,"
        "\"CurrentCarCount\":%d,"
        "\"TotalDetections\":%d,"
        "\"ProcessingFPS\":%.1f,"
        "\"AIProcessTime\":%.1f,"
        "\"SystemStatus\":\"%s\","
        "\"VideoStreamURL\":\"%s\","
        "\"timestamp\":%ld"
        "}",
        video_data.traffic_density,
        video_data.car_count,
        video_data.total_detections,
        video_data.processing_fps,
        video_data.ai_process_time,
        video_data.system_status,
        video_stream_url,
        time(NULL)
    );
    
    printf("📤 [视频数据上报] 发送到阿里云: %s\n", data_str);
    
    return demo_send_property_post(dm_handle, data_str);
}

/* 视频数据采集线程 */
void* video_data_thread(void* arg) {
    void* dm_handle = (void*)arg;
    char buffer[1024];
    int ret;
    
    printf("🎥 视频数据采集线程启动\n");
    
    while (system_running) {
        // 从Flask服务器获取数据
        ret = fetch_video_data_from_flask(buffer, sizeof(buffer));
        
        if (ret > 0) {
            // 解析并更新视频数据
            if (parse_video_data(buffer) == 0) {
                // 发送数据到阿里云
                demo_send_video_data(dm_handle);
            }
        } else {
            // 如果无法获取数据，标记为离线
            strcpy(video_data.system_status, "offline");
            printf("⚠️  无法从Flask服务器获取视频数据\n");
        }
        
        // 每5秒更新一次数据
        sleep(5);
    }
    
    printf("🎥 视频数据采集线程结束\n");
    return NULL;
}


int main(int argc, char *argv[])
{
    
    int32_t     res = STATE_SUCCESS;
    void       *dm_handle = NULL;
    void       *mqtt_handle = NULL;
    aiot_sysdep_network_cred_t cred; /* 安全凭据结构体, 如果要用TLS, 这个结构体中配置CA证书等参数 */
    uint8_t post_reply = 1;
    //对需要用到的参数初始化
    char data_str[128]={0};
    float adc = 0;
    float ath20_data[2]={0};
    int led1_state, led2_state;
    /* 打印设备信息 */
    print_device_info();
    
    /* 打印物模型配置指南 */
    print_property_model_guide();
    
    /* 配置SDK的底层依赖 */
    aiot_sysdep_set_portfile(&g_aiot_sysdep_portfile);
    /* 配置SDK的日志输出 */
    aiot_state_set_logcb(demo_state_logcb);

    /* 创建SDK的安全凭据, 用于建立TLS连接 */
    memset(&cred, 0, sizeof(aiot_sysdep_network_cred_t));
    cred.option = AIOT_SYSDEP_NETWORK_CRED_SVRCERT_CA;  /* 使用RSA证书校验MQTT服务端 */
    cred.max_tls_fragment = 16384; /* 最大的分片长度为16K, 其它可选值还有4K, 2K, 1K, 0.5K */
    cred.sni_enabled = 1;                               /* TLS建连时, 支持Server Name Indicator */
    cred.x509_server_cert = ali_ca_cert;                 /* 用来验证MQTT服务端的RSA根证书 */
    cred.x509_server_cert_len = strlen(ali_ca_cert);     /* 用来验证MQTT服务端的RSA根证书长度 */

    /* 创建1个MQTT客户端实例并内部初始化默认参数 */
    mqtt_handle = aiot_mqtt_init();
    if (mqtt_handle == NULL) {
        printf("aiot_mqtt_init failed\n");
        return -1;
    }

    /* 配置MQTT服务器地址 */
    aiot_mqtt_setopt(mqtt_handle, AIOT_MQTTOPT_HOST, (void *)mqtt_host);
    /* 配置MQTT服务器端口 */
    aiot_mqtt_setopt(mqtt_handle, AIOT_MQTTOPT_PORT, (void *)&port);
    /* 配置设备productKey */
    aiot_mqtt_setopt(mqtt_handle, AIOT_MQTTOPT_PRODUCT_KEY, (void *)product_key);
    /* 配置设备deviceName */
    aiot_mqtt_setopt(mqtt_handle, AIOT_MQTTOPT_DEVICE_NAME, (void *)device_name);
    /* 配置设备deviceSecret */
    aiot_mqtt_setopt(mqtt_handle, AIOT_MQTTOPT_DEVICE_SECRET, (void *)device_secret);
    /* 配置网络连接的安全凭据, 上面已经创建好了 */
    aiot_mqtt_setopt(mqtt_handle, AIOT_MQTTOPT_NETWORK_CRED, (void *)&cred);
    /* 配置MQTT事件回调函数 */
    aiot_mqtt_setopt(mqtt_handle, AIOT_MQTTOPT_EVENT_HANDLER, (void *)demo_mqtt_event_handler);

    /* 创建DATA-MODEL实例 */
    dm_handle = aiot_dm_init();
    if (dm_handle == NULL) {
        printf("aiot_dm_init failed");
        return -1;
    }
    /* 配置MQTT实例句柄 */
    aiot_dm_setopt(dm_handle, AIOT_DMOPT_MQTT_HANDLE, mqtt_handle);
    /* 配置消息接收处理回调函数 */
    aiot_dm_setopt(dm_handle, AIOT_DMOPT_RECV_HANDLER, (void *)demo_dm_recv_handler);

    /* 配置是云端否需要回复post_reply给设备. 如果为1, 表示需要云端回复, 否则表示不回复 */
    aiot_dm_setopt(dm_handle, AIOT_DMOPT_POST_REPLY, (void *)&post_reply);

    /* 与服务器建立MQTT连接 */
    res = aiot_mqtt_connect(mqtt_handle);
    if (res < STATE_SUCCESS) {
        /* 尝试建立连接失败, 销毁MQTT实例, 回收资源 */
        aiot_dm_deinit(&dm_handle);
        aiot_mqtt_deinit(&mqtt_handle);
        printf("aiot_mqtt_connect failed: -0x%04X\n\r\n", -res);
        printf("please check variables like mqtt_host, produt_key, device_name, device_secret in demo\r\n");
        return -1;
    }

    /* 订阅属性设置和属性上报回复的topic */
    char sub_topic1[256];
    char sub_topic2[256];
    
    // 订阅属性设置指令
    snprintf(sub_topic1, sizeof(sub_topic1), "/sys/%s/%s/thing/service/property/set", product_key, device_name);
    aiot_mqtt_sub(mqtt_handle, sub_topic1, NULL, 1, NULL);
    printf("📡 订阅属性设置: %s\n", sub_topic1);
    
    // 订阅属性上报回复
    snprintf(sub_topic2, sizeof(sub_topic2), "/sys/%s/%s/thing/event/property/post_reply", product_key, device_name);
    aiot_mqtt_sub(mqtt_handle, sub_topic2, NULL, 1, NULL);
    printf("📡 订阅上报回复: %s\n", sub_topic2);
    
    // 发送设备上线通知
    printf("📢 发送设备上线通知...\n");
    char online_msg[128];
    snprintf(online_msg, sizeof(online_msg), "{\"deviceOnline\":1,\"timestamp\":%ld}", time(NULL));
    demo_send_event_post(dm_handle, "DeviceOnline", online_msg);

    /* 创建一个单独的线程, 专用于执行aiot_mqtt_process, 它会自动发送心跳保活, 以及重发QoS1的未应答报文 */
    g_mqtt_process_thread_running = 1;
    res = pthread_create(&g_mqtt_process_thread, NULL, demo_mqtt_process_thread, mqtt_handle);
    if (res < 0) {
        printf("pthread_create demo_mqtt_process_thread failed: %d\n", res);
        aiot_dm_deinit(&dm_handle);
        aiot_mqtt_deinit(&mqtt_handle);
        return -1;
    }

    /* 创建一个单独的线程用于执行aiot_mqtt_recv, 它会循环收取服务器下发的MQTT消息, 并在断线时自动重连 */
    g_mqtt_recv_thread_running = 1;
    res = pthread_create(&g_mqtt_recv_thread, NULL, demo_mqtt_recv_thread, mqtt_handle);
    if (res < 0) {
        printf("pthread_create demo_mqtt_recv_thread failed: %d\n", res);
        aiot_dm_deinit(&dm_handle);
        aiot_mqtt_deinit(&mqtt_handle);
        return -1;
    }
   
    pthread_t video_thread;
    pthread_t video_data_collection_thread;
    TrafficLight light;
    int actual_green_time;
    int ret;
    // 初始化信号量
    sem_init(&data_mutex, 0, 1);
    
   
    // 初始化交通灯控制
    ret = init_traffic_light(&light);
    if (ret != 0) {
        printf("初始化交通灯控制失败\n");
        return 1;
    }
    
    // 创建视频数据采集线程 (传递dm_handle而不是未初始化的aliyun_handle)
    ret = pthread_create(&video_thread, NULL, video_data_collection, dm_handle);
    if (ret != 0) {
        printf("创建视频数据采集线程失败\n");
        return 1;
    }
    
    // 创建从Flask服务器获取视频数据的线程
    ret = pthread_create(&video_data_collection_thread, NULL, video_data_thread, dm_handle);
    if (ret != 0) {
        printf("创建Flask视频数据采集线程失败\n");
        return 1;
    }
    
    printf("智能交通灯系统已启动\n");
    
    // 发送测试数据验证物模型
    printf("\n🧪 发送测试数据验证物模型配置...\n");
    char test_data[256];
    
    // 测试1: 简单属性
    printf("测试1: 发送简单LED状态...\n");
    sprintf(test_data, "{\"RedLED\":1,\"YellowLED\":0,\"GreenLED\":0}");
    demo_send_property_post(dm_handle, test_data);
    sleep(2);
    
    // 测试2: 交通数据
    printf("测试2: 发送交通数据...\n");
    sprintf(test_data, "{\"TrafficDensity\":0.75,\"IllegalVehicles\":2,\"GreenExtension\":5}");
    demo_send_property_post(dm_handle, test_data);
    
    printf("⏳ 等待5秒查看阿里云平台是否接收到数据...\n");
    sleep(5);
    
    // 主循环 - 控制交通灯
    while (system_running) {
        // 红灯阶段
        printf("红灯亮起 (30秒)\n");
        set_light(&light, 1, 0, 0);
        
        memset(data_str, 0, sizeof(data_str));
        sprintf(data_str, "{\"RedLED\":1,\"YellowLED\":0,\"GreenLED\":0}");
        demo_send_property_post(dm_handle, data_str);
        
        sleep(30);
        
        // 获取当前绿灯延长时间
        sem_wait(&data_mutex);
        actual_green_time = NORMAL_GREEN_TIME + green_extension;
        sem_post(&data_mutex);
        
        // 绿灯阶段
        printf("绿灯亮起 (持续 %d 秒)\n", actual_green_time);
        set_light(&light, 0, 0, 1);
        
        memset(data_str, 0, sizeof(data_str));
        sprintf(data_str, "{\"RedLED\":0,\"YellowLED\":0,\"GreenLED\":1,\"GreenTime\":%d}", actual_green_time);
        demo_send_property_post(dm_handle, data_str);
        
        sleep(actual_green_time);
        
        // 黄灯阶段
        printf("黄灯亮起 (3秒)\n");
        set_light(&light, 0, 1, 0);
        
        memset(data_str, 0, sizeof(data_str));
        sprintf(data_str, "{\"RedLED\":0,\"YellowLED\":1,\"GreenLED\":0}");
        demo_send_property_post(dm_handle, data_str);
        
        sleep(3);
    }
    
    // 清理资源
    printf("系统正在关闭...\n");
    system_running = 0;
    
    // 等待所有线程结束
    pthread_join(video_thread, NULL);
    pthread_join(video_data_collection_thread, NULL);
    
    sem_destroy(&data_mutex);
    close_traffic_light(&light);
    
    printf("智能交通灯系统已关闭\n");
    printf("开始交通灯循环...\n");
    /* 主循环进入休眠 */
    while (1) {
        /* TODO: 以下代码演示了简单的属性上报和事件上报, 用户可取消注释观察演示效果 */
         printf("红灯亮\n");
        set_light(&light, 1, 0, 0);
        memset(data_str, 0, sizeof(data_str));
        sprintf(data_str, "{\"RedLED\": 1, \"YellowLED\": 0, \"GreenLED\": 0}");
        demo_send_property_post(dm_handle, data_str);
        sleep(30);

        // 绿灯亮（通行25秒）
        printf("绿灯亮\n"); 
        set_light(&light, 0, 0, 1);
        memset(data_str, 0, sizeof(data_str));
        sprintf(data_str, "{\"RedLED\": 0, \"YellowLED\": 0, \"GreenLED\": 1}");
        demo_send_property_post(dm_handle, data_str);
        sleep(25);

        // 黄灯亮（警示3秒）
        printf("黄灯亮\n");
        set_light(&light, 0, 1, 0);
        memset(data_str, 0, sizeof(data_str));
        sprintf(data_str, "{\"RedLED\": 0, \"YellowLED\": 1, \"GreenLED\": 0}");
        demo_send_property_post(dm_handle, data_str);
        sleep(3);

        close_traffic_light(&light);
        /*
        demo_send_event_post(dm_handle, "Error", "{\"ErrorCode\": 0}");
        */

        /* TODO: 以下代码演示了基于模块的物模型的上报, 用户可取消注释观察演示效果
         * 本例需要用户在产品的功能定义的页面中, 点击"编辑草稿", 增加一个名为demo_extra_block的模块,
         * 再到该模块中, 通过添加标准功能, 选择一个名为NightLightSwitch的物模型属性, 再点击"发布上线".
         * 有关模块化的物模型的概念, 请见 https://help.aliyun.com/document_detail/73727.html
        */
        /*
        demo_send_property_post(dm_handle, "{\"demo_extra_block:NightLightSwitch\": 1}");
        */

        /* TODO: 以下代码显示批量上报用户数据, 用户可取消注释观察演示效果
         * 具体数据格式请见https://help.aliyun.com/document_detail/89301.html 的"设备批量上报属性、事件"一节
        */
       /*
        demo_send_property_batch_post(dm_handle,
                                      "{\"properties\":{\"LEDSwitch\": [ {\"value\":\"on\"],\"temperature\": [{\"value\": 19.8]}}");
        */
        sleep(5);
    }

    /* 停止收发动作 */
    g_mqtt_process_thread_running = 0;
    g_mqtt_recv_thread_running = 0;

    /* 断开MQTT连接, 一般不会运行到这里 */
    res = aiot_mqtt_disconnect(mqtt_handle);
    if (res < STATE_SUCCESS) {
        aiot_dm_deinit(&dm_handle);
        aiot_mqtt_deinit(&mqtt_handle);
        printf("aiot_mqtt_disconnect failed: -0x%04X\n", -res);
        return -1;
    }

    /* 销毁DATA-MODEL实例, 一般不会运行到这里 */
    res = aiot_dm_deinit(&dm_handle);
    if (res < STATE_SUCCESS) {
        printf("aiot_dm_deinit failed: -0x%04X\n", -res);
        return -1;
    }

    /* 销毁MQTT实例, 一般不会运行到这里 */
    res = aiot_mqtt_deinit(&mqtt_handle);
    if (res < STATE_SUCCESS) {
        printf("aiot_mqtt_deinit failed: -0x%04X\n", -res);
        return -1;
    }

    pthread_join(g_mqtt_process_thread, NULL);
    pthread_join(g_mqtt_recv_thread, NULL);

    return 0;
}

