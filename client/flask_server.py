import json
import cv2
import threading
import time
import subprocess
import queue
import base64
import numpy as np
from flask import Flask, jsonify, request, Response, render_template_string
from flask import render_template
from alibabacloud_tea_openapi.client import Client as OpenApiClient
from alibabacloud_tea_openapi import models as open_api_models
from alibabacloud_tea_util import models as util_models
from alibabacloud_openapi_util.client import Client as OpenApiUtilClient
from shared_memory_dict import SharedMemoryDict

web = Flask(__name__)

# 阿里云API配置（恢复原配置）
access_key_id = "xxxxxxxxxxxxxxxx"
access_key_secret = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"

# 创建一个共享内存字典，用于存储设备信息
shared_device_info = SharedMemoryDict(name='device_info', size=1024)

# 视频流相关配置
RTMP_PULL_URL = "rtmp://localhost:1935/live/stream"
video_frame_queue = queue.Queue(maxsize=10)
latest_detection_data = {'timestamp': 0, 'car_count': 0, 'total_detections': 0}
video_thread_running = False

data_model_process = None  # 全局变量，保存进程对象

class RTMPVideoCapture:
    """RTMP视频拉流器"""
    def __init__(self, rtmp_url):
        self.rtmp_url = rtmp_url
        self.cap = None
        self.running = False
        
    def start_capture(self):
        """启动RTMP拉流"""
        try:
            # 使用FFmpeg拉流
            command = [
                'ffmpeg',
                '-i', self.rtmp_url,
                '-f', 'rawvideo',
                '-pix_fmt', 'bgr24',
                '-an',  # 不需要音频
                '-'
            ]
            process = subprocess.Popen("./home/elf/ELFboard_IOT_monitor_system-main/client/flask_server.py", stdout=subprocess.PIPE, text=True)

            # Python程序可以继续执行其他任务
            for i in range(3):
                print("Python程序运行中: ", i)
                time.sleep(0.5)
            self.process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=10**8
            )
            
            self.running = True
            print(f"RTMP拉流已启动: {self.rtmp_url}")
            return True
            
        except Exception as e:
            print(f"RTMP拉流启动失败: {e}")
            return False
    
    def read_frame(self, width=640, height=480):
        """读取视频帧"""
        if not self.running or not self.process or not self.process.stdout:
            return None
            
        try:
            # 计算帧大小
            frame_size = width * height * 3  # BGR 3通道
            
            # 读取原始帧数据
            raw_frame = self.process.stdout.read(frame_size)
            
            if len(raw_frame) != frame_size:
                return None
            
            # 转换为numpy数组
            frame = np.frombuffer(raw_frame, dtype=np.uint8)
            frame = frame.reshape((height, width, 3))
            
            return frame
            
        except Exception as e:
            print(f"读取帧失败: {e}")
            return None
    
    def stop_capture(self):
        """停止拉流"""
        if self.running and self.process:
            self.process.terminate()
            self.process.wait()
            self.running = False
            print("RTMP拉流已停止")

class VideoStreamManager:
    """视频流管理器"""
    def __init__(self):
        self.rtmp_capture = None
        self.backup_capture = None  # 备用摄像头
        self.current_frame = None
        self.frame_lock = threading.Lock()
        
    def start_video_stream(self):
        """启动视频流"""
        global video_thread_running
        
        # 尝试启动RTMP拉流
        self.rtmp_capture = RTMPVideoCapture(RTMP_PULL_URL)
        
        if not self.rtmp_capture.start_capture():
            print("RTMP拉流失败，尝试使用备用摄像头")
            # 尝试使用本地摄像头作为备用
            self.backup_capture = cv2.VideoCapture(0)
            if not self.backup_capture.isOpened():
                print("无法启动任何视频源")
                return False
        
        video_thread_running = True
        
        # 启动视频处理线程
        video_thread = threading.Thread(target=self._video_processing_loop)
        video_thread.daemon = True
        video_thread.start()
        
        print("视频流管理器已启动")
        return True
    
    def _video_processing_loop(self):
        """视频处理循环"""
        while video_thread_running:
            frame = None
            
            # 优先使用RTMP流
            if self.rtmp_capture and self.rtmp_capture.running:
                frame = self.rtmp_capture.read_frame()
            
            # 如果RTMP流失败，使用备用摄像头
            if frame is None and self.backup_capture:
                ret, frame = self.backup_capture.read()
                if not ret:
                    frame = None
            
            if frame is not None:
                with self.frame_lock:
                    self.current_frame = frame.copy()
                    
                # 将帧放入队列（用于其他处理）
                try:
                    video_frame_queue.put(frame, timeout=0.1)
                except queue.Full:
                    # 队列满了，丢弃最旧的帧
                    try:
                        video_frame_queue.get_nowait()
                        video_frame_queue.put(frame, timeout=0.1)
                    except queue.Empty:
                        pass
            
            time.sleep(0.033)  # 约30 FPS
    
    def get_current_frame(self):
        """获取当前帧"""
        with self.frame_lock:
            return self.current_frame.copy() if self.current_frame is not None else None
    
    def stop_video_stream(self):
        """停止视频流"""
        global video_thread_running
        video_thread_running = False
        
        if self.rtmp_capture:
            self.rtmp_capture.stop_capture()
        
        if self.backup_capture:
            self.backup_capture.release()

# 创建视频流管理器
video_manager = VideoStreamManager()


class Sample:
    def __init__(self):
        pass

    @staticmethod
    def create_client(
            access_key_id: str,
            access_key_secret: str,
    ) -> OpenApiClient:
        """
        使用AK&SK初始化账号Client
        @param access_key_id:
        @param access_key_secret:
        @return: Client
        @throws Exception
        """
        config = open_api_models.Config(
            # 必填，您的 AccessKey ID,
            access_key_id=access_key_id,
            # 必填，您的 AccessKey Secret,
            access_key_secret=access_key_secret
        )
        # 使用上海区域的endpoint，适用于公共实例
        config.endpoint = f'iot.cn-shanghai.aliyuncs.com'
        return OpenApiClient(config)

    @staticmethod
    def create_set_info() -> open_api_models.Params:
        """
        API 相关
        @param path: params
        @return: OpenApi.Params
        """
        params = open_api_models.Params(
            # 接口名称,
            action='SetDeviceProperty',
            # 接口版本,
            version='2018-01-20',
            # 接口协议,
            protocol='HTTPS',
            # 接口 HTTP 方法,
            method='POST',
            auth_type='AK',
            style='RPC',
            # 接口 PATH,
            pathname=f'/',
            # 接口请求体内容格式,
            req_body_type='formData',
            # 接口响应体内容格式,
            body_type='json'
        )
        return params

    @staticmethod
    def create_get_info() -> open_api_models.Params:
        """
        API 相关
        @param path: params
        @return: OpenApi.Params
        """
        params = open_api_models.Params(
            # 接口名称,
            action='QueryDeviceOriginalPropertyStatus',
            # 接口版本,
            version='2018-01-20',
            # 接口协议,
            protocol='HTTPS',
            # 接口 HTTP 方法,
            method='POST',
            auth_type='AK',
            style='RPC',
            # 接口 PATH,
            pathname=f'/',
            # 接口请求体内容格式,
            req_body_type='formData',
            # 接口响应体内容格式,
            body_type='json'
        )
        return params

    @staticmethod
    def main():
        try:
            print("[API数据] 开始获取IoT设备数据...")
            
            # 创建阿里云IoT客户端
            client = Sample.create_client(access_key_id, access_key_secret)
            params = Sample.create_get_info()
            
            # 构建请求体
            queries = {}
            queries['ProductKey'] = 'k260tC1CBFv'
            queries['DeviceName'] = 'control_unit_1'
            queries['PageSize'] = 50  # 添加必需的PageSize参数
            queries['CurrentPage'] = 1  # 添加当前页参数
            queries['Asc'] = 1  # 添加必需的排序参数，1表示升序
            # queries['IotInstanceId'] = 'YOUR_INSTANCE_ID'  # 需要替换为你的实际实例ID
            
            # 发送请求获取设备数据
            request = open_api_models.OpenApiRequest(
                query=OpenApiUtilClient.query(queries)
            )
            runtime = util_models.RuntimeOptions()
            
            # 发送API请求
            try:
                response = client.call_api(params, request, runtime)
                result = response.get('body')
                
                print(f"[API响应] 完整响应数据: {result}")
                
                # 解析响应数据
                if result and 'Data' in result:
                    data = result['Data']
                    print(f"[API响应] Data字段内容: {data}")
                    
                    # 检查List字段是否存在且为字典
                    if 'List' in data and isinstance(data['List'], dict):
                        device_list = data['List']
                        print(f"[API响应] List字段内容: {device_list}")
                        
                        # 构建标准化的消息格式
                        message = {
                            'humi': device_list.get('humidity', 0.0),
                            'temp': device_list.get('temperature', 0.0),
                            'volt': device_list.get('voltage', 0.0),
                            'led1': device_list.get('led1', 0),
                            'led2': device_list.get('led2', 0),
                            'timestamp': time.time(),
                            'status': 'online'
                        }
                    elif 'List' in data and isinstance(data['List'], list) and len(data['List']) > 0:
                        # 如果List是数组，取第一个元素
                        device_list = data['List'][0]
                        print(f"[API响应] List数组第一个元素: {device_list}")
                        
                        message = {
                            'humi': device_list.get('humidity', 0.0),
                            'temp': device_list.get('temperature', 0.0),
                            'volt': device_list.get('voltage', 0.0),
                            'led1': device_list.get('led1', 0),
                            'led2': device_list.get('led2', 0),
                            'timestamp': time.time(),
                            'status': 'online'
                        }
                    else:
                        print(f"[API响应] List字段格式不正确或为空: {data.get('List', 'Not Found')}")
                        # 如果没有有效的List数据，返回默认值
                        message = {
                            'humi': 0.0,
                            'temp': 0.0,
                            'volt': 0.0,
                            'led1': 0,
                            'led2': 0,
                            'timestamp': time.time(),
                            'status': 'offline'
                        }
                else:
                    print(f"[API响应] 没有找到Data字段，完整结果: {result}")
                    # 如果没有数据，返回默认值
                    message = {
                        'humi': 0.0,
                        'temp': 0.0,
                        'volt': 0.0,
                        'led1': 0,
                        'led2': 0,
                        'timestamp': time.time(),
                        'status': 'offline'
                    }
                
                print(f"[成功] 设备数据: {message}")
                
                # 将设备信息存储到共享内存字典中
                for key, value in message.items():
                    shared_device_info[key] = value
                    
                return message
                
            except Exception as api_error:
                print(f"[API错误] 调用失败: {api_error}")
                
                # 返回默认数据，确保始终返回有效的字典
                default_message = {
                    'humi': 0.0,
                    'temp': 0.0,
                    'volt': 0.0,
                    'led1': 0,
                    'led2': 0,
                    'timestamp': time.time(),
                    'error': str(api_error),
                    'status': 'offline'
                }
                
                # 存储到共享内存
                for key, value in default_message.items():
                    shared_device_info[key] = value
                    
                return default_message
                
        except Exception as e:
            print(f"[异常] API数据获取失败: {e}")
            print(f"[异常] 异常类型: {type(e).__name__}")
            
            # 返回默认数据，确保始终返回有效的字典
            default_message = {
                'humi': 0.0,
                'temp': 0.0,
                'volt': 0.0,
                'led1': 0,
                'led2': 0,
                'timestamp': time.time(),
                'error': str(e),
                'status': 'offline'
            }
            
            # 存储到共享内存
            for key, value in default_message.items():
                shared_device_info[key] = value
                
            return default_message

    @staticmethod
    def main_set(item: str):
        try:
            print(f"[API控制] 开始设置LED状态: {item}")
            
            # 创建阿里云IoT客户端
            client = Sample.create_client(access_key_id, access_key_secret)
            params = Sample.create_set_info()
            
            # 解析控制指令
            control_data = json.loads(item)
            
            # 构建请求体
            queries = {}
            queries['ProductKey'] = 'k260tC1CBFv'
            queries['DeviceName'] = 'control_unit_1'
            queries['Items'] = item  # 直接传递JSON字符串
            # queries['IotInstanceId'] = 'YOUR_INSTANCE_ID'  # 需要替换为你的实际实例ID
            
            # 发送请求
            request = open_api_models.OpenApiRequest(
                query=OpenApiUtilClient.query(queries)
            )
            runtime = util_models.RuntimeOptions()
            
            # 发送API请求
            try:
                response = client.call_api(params, request, runtime)
                result = response.get('body')
                
                print(f"[API控制] LED控制成功: {result}")
                
                # 更新本地状态
                for key, value in control_data.items():
                    if key == 'LEDSwitch':
                        shared_device_info['led1'] = value
                    elif key == 'LEDSwitch2':
                        shared_device_info['led2'] = value
                
                return str(True)
                
            except Exception as api_error:
                print(f"[API控制] LED控制失败: {api_error}")
                return str(False)
            
        except Exception as e:
            print(f"[API控制] LED控制失败: {e}")
            return str(False)


@web.route('/')
def movie_list():
    return render_template('index_enhanced.html')


@web.route('/get_data', methods=['GET', 'POST'])
def api_data():
    try:
        print("[路由] 开始获取IoT数据...")
        message = Sample.main()
        
        # 确保返回的数据是字典类型
        if not isinstance(message, dict):
            print(f"[路由] 返回数据类型错误: {type(message)}")
            raise ValueError("API返回数据格式错误")
            
        print(f"[路由] 成功获取数据: {message}")
        return jsonify(message)
        
    except Exception as e:
        print(f"[路由] 获取IoT数据失败: {e}")
        print(f"[路由] 异常类型: {type(e).__name__}")
        
        # 返回默认的JSON数据而不是HTML错误页面
        error_response = {
            'humi': 0.0,
            'temp': 0.0,
            'volt': 0.0,
            'led1': 0,
            'led2': 0,
            'timestamp': time.time(),
            'error': str(e),
            'status': 'error'
        }
        
        print(f"[路由] 返回错误响应: {error_response}")
        return jsonify(error_response), 500


@web.route('/set_data', methods=['GET', 'POST'])
def api_set():
    name = request.form.get('name')
    state = request.form.get('state')

    request_info = {}
    data = json.loads(json.dumps(request_info))
    data[name] = int(state) # type: ignore
    request_data = json.dumps(data, ensure_ascii=False)
    print(request_data)

    message = Sample.main_set(request_data)
    return message


@web.route('/video_data', methods=['POST'])
def receive_video_data():
    """接收来自检测脚本的数据"""
    global latest_detection_data
    
    try:
        data = request.get_json()
        if data:
            latest_detection_data = data
            print(f"收到检测数据: 车辆数={data.get('car_count', 0)}, 时间={data.get('timestamp', 0)}")
            
            # 将数据转发到阿里云物联网平台
            # 这里可以添加转发逻辑
            
            return jsonify({'status': 'success', 'message': 'Data received'})
        else:
            return jsonify({'status': 'error', 'message': 'No data received'}), 400
            
    except Exception as e:
        print(f"接收数据失败: {e}")
        return jsonify({'status': 'error', 'message': str(e)}), 500


def generate_video_stream():
    """生成视频流"""
    while True:
        frame = video_manager.get_current_frame()
        
        if frame is not None:
            # 添加检测信息到帧上
            try:
                # 添加检测信息文本
                info_text = f"车辆数: {latest_detection_data.get('car_count', 0)}"
                total_text = f"总检测: {latest_detection_data.get('total_detections', 0)}"
                fps_text = f"FPS: {latest_detection_data.get('frame_fps', 0):.1f}"
                
                cv2.putText(frame, info_text, (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                cv2.putText(frame, total_text, (10, 60), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                cv2.putText(frame, fps_text, (10, 90), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
                
                # 编码帧
                ret, buffer = cv2.imencode('.jpg', frame)
                if ret:
                    frame_bytes = buffer.tobytes()
                    yield (b'--frame\r\n'
                           b'Content-Type: image/jpeg\r\n\r\n' + frame_bytes + b'\r\n')
                
            except Exception as e:
                print(f"视频流生成错误: {e}")
        
        time.sleep(0.033)  # 约30 FPS


@web.route('/video_stream')
def video_stream():
    """视频流端点"""
    return Response(
        generate_video_stream(),
        mimetype='multipart/x-mixed-replace; boundary=frame'
    )


@web.route('/video_dashboard')
def video_dashboard():
    """视频监控仪表板"""
    html_template = """
    <!DOCTYPE html>
    <html>
    <head>
        <title>智能交通监控系统</title>
        <meta charset="utf-8">
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
            body {
                font-family: Arial, sans-serif;
                margin: 0;
                padding: 20px;
                background-color: #f0f0f0;
            }
            .container {
                max-width: 1200px;
                margin: 0 auto;
                background-color: white;
                padding: 20px;
                border-radius: 10px;
                box-shadow: 0 2px 10px rgba(0,0,0,0.1);
            }
            .header {
                text-align: center;
                color: #333;
                margin-bottom: 20px;
            }
            .video-container {
                text-align: center;
                margin-bottom: 20px;
            }
            .video-stream {
                max-width: 100%;
                height: auto;
                border: 2px solid #333;
                border-radius: 10px;
            }
            .stats-container {
                display: grid;
                grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
                gap: 20px;
                margin-bottom: 20px;
            }
            .stat-card {
                background-color: #f8f9fa;
                padding: 20px;
                border-radius: 10px;
                text-align: center;
                border: 1px solid #dee2e6;
            }
            .stat-value {
                font-size: 2em;
                font-weight: bold;
                color: #007bff;
            }
            .stat-label {
                color: #6c757d;
                margin-top: 10px;
            }
            .controls {
                text-align: center;
                margin-top: 20px;
            }
            .btn {
                background-color: #007bff;
                color: white;
                border: none;
                padding: 10px 20px;
                border-radius: 5px;
                cursor: pointer;
                margin: 0 10px;
            }
            .btn:hover {
                background-color: #0056b3;
            }
            .status-indicator {
                display: inline-block;
                width: 12px;
                height: 12px;
                border-radius: 50%;
                margin-right: 5px;
            }
            .status-online {
                background-color: #28a745;
            }
            .status-offline {
                background-color: #dc3545;
            }
        </style>
    </head>
    <body>
        <div class="container">
            <div class="header">
                <h1>智能交通监控系统</h1>
                <p>实时车辆检测与流量分析</p>
            </div>
            
            <div class="video-container">
                <img src="/video_stream" class="video-stream" alt="视频流">
            </div>
            
            <div class="stats-container">
                <div class="stat-card">
                    <div class="stat-value" id="car-count">0</div>
                    <div class="stat-label">当前车辆数</div>
                </div>
                <div class="stat-card">
                    <div class="stat-value" id="total-detections">0</div>
                    <div class="stat-label">总检测次数</div>
                </div>
                <div class="stat-card">
                    <div class="stat-value" id="fps">0.0</div>
                    <div class="stat-label">处理FPS</div>
                </div>
                <div class="stat-card">
                    <div class="stat-value" id="process-time">0.0</div>
                    <div class="stat-label">处理时间(ms)</div>
                </div>
            </div>
            
            <div class="controls">
                <span class="status-indicator" id="status-indicator"></span>
                <span id="status-text">连接状态</span>
                <br><br>
                <button class="btn" onclick="refreshData()">刷新数据</button>
                <button class="btn" onclick="resetStats()">重置统计</button>
            </div>
        </div>
        
        <script>
            let isOnline = false;
            
            function updateStats() {
                fetch('/get_detection_data')
                    .then(response => response.json())
                    .then(data => {
                        document.getElementById('car-count').textContent = data.car_count || 0;
                        document.getElementById('total-detections').textContent = data.total_detections || 0;
                        document.getElementById('fps').textContent = (data.frame_fps || 0).toFixed(1);
                        document.getElementById('process-time').textContent = (data.ai_process_time || 0).toFixed(1);
                        
                        // 更新状态指示器
                        const indicator = document.getElementById('status-indicator');
                        const statusText = document.getElementById('status-text');
                        
                        if (data.timestamp && Date.now() - data.timestamp * 1000 < 10000) {
                            indicator.className = 'status-indicator status-online';
                            statusText.textContent = '检测系统在线';
                            isOnline = true;
                        } else {
                            indicator.className = 'status-indicator status-offline';
                            statusText.textContent = '检测系统在线';
                            isOnline = false;
                        }
                    })
                    .catch(error => {
                        console.error('获取数据失败:', error);
                        document.getElementById('status-indicator').className = 'status-indicator status-offline';
                        document.getElementById('status-text').textContent = '连接失败';
                        isOnline = false;
                    });
            }
            
            function refreshData() {
                updateStats();
            }
            
            function resetStats() {
                if (confirm('确定要重置所有统计数据吗？')) {
                    fetch('/reset_stats', {method: 'POST'})
                        .then(response => response.json())
                        .then(data => {
                            alert('统计数据已重置');
                            updateStats();
                        })
                        .catch(error => {
                            alert('重置失败: ' + error);
                        });
                }
            }
            
            // 定时更新数据
            setInterval(updateStats, 1000);
            
            // 页面加载时更新一次
            updateStats();
        </script>
    </body>
    </html>
    """
    return render_template_string(html_template)


@web.route('/get_detection_data')
def get_detection_data():
    """获取检测数据"""
    return jsonify(latest_detection_data)


@web.route('/reset_stats', methods=['POST'])
def reset_stats():
    """重置统计数据"""
    global latest_detection_data
    latest_detection_data = {
        'timestamp': time.time(),
        'car_count': 0,
        'total_detections': 0,
        'frame_fps': 0,
        'ai_process_time': 0
    }
    return jsonify({'status': 'success', 'message': 'Stats reset'})


@web.route('/read_c_file')
def read_c_file():
    file_path = '../board/MQTT/demos/data_model_basic_demo.c'  # 路径根据实际情况调整
    try:
        with open(file_path, 'r', encoding='utf-8') as f:
            content = f.read()
        return jsonify({'status': 'success', 'content': content})
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)})


@web.route('/start_data_model', methods=['POST'])
def start_data_model():
    global data_model_process
    if data_model_process is not None and data_model_process.poll() is None:
        return jsonify({'status': 'already running'})
    # 启动可执行文件
    data_model_process = subprocess.Popen(
        ["../board/MQTT/output/data-model-basic-demo"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    return jsonify({'status': 'started'})

@web.route('/stop_data_model', methods=['POST'])
def stop_data_model():
    global data_model_process
    if data_model_process is not None and data_model_process.poll() is None:
        data_model_process.terminate()
        data_model_process.wait()
        return jsonify({'status': 'stopped'})
    return jsonify({'status': 'not running'})

@web.route('/data_model_status', methods=['GET'])
def data_model_status():
    global data_model_process
    if data_model_process is not None and data_model_process.poll() is None:
        return jsonify({'status': 'running'})
    return jsonify({'status': 'not running'})


if __name__ == '__main__':
    # 启动data-model-basic-demo可执行文件
    import subprocess
    data_model_process = subprocess.Popen(
        ["../board/MQTT/output/data-model-basic-demo"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE
    )
    print("已自动启动 data-model-basic-demo 可执行文件，进程PID:", data_model_process.pid)
    # 启动Flask服务
    web.run(host='0.0.0.0', port=5000, debug=True)