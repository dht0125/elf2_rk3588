#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import time
import sys
import numpy as np
import cv2
import threading
import queue
from fcntl import ioctl
import struct
from collections import deque

# 配置参数
CAMERA_DEVICE = '/dev/video21'  # 摄像头设备
CLASSES = ["car"]

# 多线程性能参数
FRAME_QUEUE_SIZE = 5      # 帧队列大小
FRAME_SKIP = 1            # 多线程模式下减少跳帧
MAX_FPS = 30             # 目标最大FPS

# 全局线程控制变量
stop_threads = threading.Event()
frame_lock = threading.Lock()

class ThreadSafeQueue:
    """线程安全的队列封装"""
    def __init__(self, maxsize=0):
        self.queue = queue.Queue(maxsize=maxsize)
        
    def put(self, item, timeout=None):
        try:
            self.queue.put(item, block=True, timeout=timeout)
            return True
        except queue.Full:
            return False
            
    def get(self, timeout=None):
        try:
            return self.queue.get(block=True, timeout=timeout), True
        except queue.Empty:
            return None, False
            
    def empty(self):
        return self.queue.empty()
        
    def qsize(self):
        return self.queue.qsize()

class FrameReader(threading.Thread):
    """摄像头帧读取线程"""
    def __init__(self, cap, frame_queue):
        super().__init__(daemon=True)
        self.cap = cap
        self.frame_queue = frame_queue
        self.fps_counter = 0
        self.last_fps_time = time.time()
        self.read_fps = 0
        
    def run(self):
        print("摄像头读取线程启动...")
        frame_interval = 1.0 / MAX_FPS  # 控制读取帧率
        last_frame_time = time.time()
        
        while not stop_threads.is_set():
            current_time = time.time()
            
            # 控制读取帧率
            if current_time - last_frame_time < frame_interval:
                time.sleep(0.001)  # 短暂休眠
                continue
                
            ret, frame = self.cap.read()
            if not ret:
                print("摄像头读取失败")
                stop_threads.set()
                break
                
            # 更新FPS统计
            self.fps_counter += 1
            if current_time - self.last_fps_time >= 1.0:
                self.read_fps = self.fps_counter / (current_time - self.last_fps_time)
                self.fps_counter = 0
                self.last_fps_time = current_time
            
            # 将帧放入队列（如果队列满了，丢弃最旧的帧）
            if not self.frame_queue.put((frame.copy(), current_time), timeout=0.001):
                # 队列满了，清空队列并放入新帧
                while not self.frame_queue.empty():
                    self.frame_queue.get(timeout=0.001)
                self.frame_queue.put((frame.copy(), current_time), timeout=0.001)
            
            last_frame_time = current_time
            
        print("摄像头读取线程结束")







def draw_detections(image, boxes, scores, classes):
    """在图像上绘制检测结果"""
    if boxes is None or len(boxes) == 0:
        return 0
    
    detection_count = 0
    try:
        for i, (box, score, cl) in enumerate(zip(boxes, scores, classes)):
            # 检查类别索引是否有效
            if cl >= len(CLASSES) or cl < 0:
                print(f"警告: 无效的类别索引 {cl}，跳过该检测框")
                continue
                
            # 获取边界框坐标（已经是原始图像坐标）
            x1, y1, x2, y2 = box
            
            # 转换为整数坐标并限制在图像范围内
            x1 = int(max(0, x1))
            y1 = int(max(0, y1))
            x2 = int(min(image.shape[1], x2))
            y2 = int(min(image.shape[0], y2))
            
            # 检查坐标是否有效
            if x2 <= x1 or y2 <= y1:
                print(f"警告: 无效的检测框坐标 ({x1},{y1},{x2},{y2})，跳过")
                continue
            
            # 绘制检测框
            cv2.rectangle(image, (x1, y1), (x2, y2), (0, 255, 0), 2)
            
            # 绘制标签
            try:
                label = f'{CLASSES[cl]} {score:.2f}'
                label_size = cv2.getTextSize(label, cv2.FONT_HERSHEY_SIMPLEX, 0.6, 2)[0]
                
                # 绘制标签背景
                cv2.rectangle(image, (x1, y1 - label_size[1] - 10), 
                             (x1 + label_size[0], y1), (0, 255, 0), -1)
                
                # 绘制标签文字
                cv2.putText(image, label, (x1, y1 - 5),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 2)
            except Exception as e:
                print(f"绘制标签时出错: {e}")
                continue
            
            detection_count += 1
            
    except Exception as e:
        print(f"绘制检测结果时发生错误: {e}")
        print(f"boxes长度: {len(boxes) if boxes is not None else 'None'}")
        print(f"scores长度: {len(scores) if scores is not None else 'None'}")
        print(f"classes长度: {len(classes) if classes is not None else 'None'}")
        return 0
    
    return detection_count


class CarDetector:
    def __init__(self):
        """初始化传统车辆检测器"""
        self.background_subtractor = cv2.createBackgroundSubtractorMOG2(
            detectShadows=True,
            varThreshold=50,
            history=500
        )
        self.kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
        self.car_cascade = None
        self.min_car_area = 2000  # 最小车辆面积
        self.max_car_area = 50000  # 最大车辆面积
        self.detection_initialized = False
        
        # 尝试加载Haar级联分类器（如果可用）
        try:
            # 尝试常见的Haar级联分类器路径
            possible_paths = [
                'haarcascade_car.xml',  # 自定义车辆分类器
                '/usr/share/opencv/haarcascades/haarcascade_car.xml',  # 系统路径
                './haarcascade_car.xml'  # 当前目录
            ]
            
            self.car_cascade = None
            for path in possible_paths:
                try:
                    cascade = cv2.CascadeClassifier(path)
                    if not cascade.empty():
                        self.car_cascade = cascade
                        print(f"Haar级联分类器加载成功: {path}")
                        break
                except:
                    continue
            
            if self.car_cascade is None:
                print("未找到Haar级联分类器，使用运动检测方法")
        except:
            self.car_cascade = None
            print("未找到Haar级联分类器，使用运动检测方法")
        
        print("传统车辆检测器初始化完成")
    
    def detect(self, image):
        """检测图像中的车辆"""
        try:
            # 预处理图像
            gray = cv2.cvtColor(image, cv2.COLOR_BGR2GRAY)
            blurred = cv2.GaussianBlur(gray, (5, 5), 0)
            
            # 方法1：使用Haar级联分类器（如果可用）
            if self.car_cascade is not None:
                cars = self.car_cascade.detectMultiScale(
                    blurred,
                    scaleFactor=1.1,
                    minNeighbors=3,
                    minSize=(50, 50),
                    maxSize=(300, 300)
                )
                
                if len(cars) > 0:
                    # 转换为标准格式
                    boxes = []
                    scores = []
                    classes = []
                    
                    for (x, y, w, h) in cars:
                        # 转换为[x1, y1, x2, y2]格式
                        boxes.append([x, y, x + w, y + h])
                        scores.append(0.8)  # 固定置信度
                        classes.append(0)   # 车辆类别
                    
                    return np.array(boxes), np.array(classes), np.array(scores)
            
            # 方法2：使用背景减除和运动检测
            fg_mask = self.background_subtractor.apply(blurred)
            
            # 形态学操作去除噪声
            fg_mask = cv2.morphologyEx(fg_mask, cv2.MORPH_OPEN, self.kernel)
            fg_mask = cv2.morphologyEx(fg_mask, cv2.MORPH_CLOSE, self.kernel)
            
            # 膨胀操作连接相邻区域
            fg_mask = cv2.dilate(fg_mask, self.kernel, iterations=2)
            
            # 查找轮廓
            contours, _ = cv2.findContours(fg_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
            
            # 过滤轮廓
            boxes = []
            scores = []
            classes = []
            
            for contour in contours:
                area = cv2.contourArea(contour)
                
                # 根据面积过滤
                if area < self.min_car_area or area > self.max_car_area:
                    continue
                
                # 获取边界框
                x, y, w, h = cv2.boundingRect(contour)
                
                # 宽高比过滤（车辆通常比较宽）
                aspect_ratio = w / h
                if aspect_ratio < 0.5 or aspect_ratio > 4.0:
                    continue
                
                # 计算填充比例（轮廓面积与边界框面积的比例）
                rect_area = w * h
                fill_ratio = area / rect_area
                if fill_ratio < 0.3:  # 过滤掉太稀疏的区域
                    continue
                
                # 添加检测结果
                boxes.append([x, y, x + w, y + h])
                scores.append(min(0.9, area / 10000))  # 根据面积计算置信度
                classes.append(0)  # 车辆类别
            
            if len(boxes) > 0:
                return np.array(boxes), np.array(classes), np.array(scores)
            else:
                return None, None, None
                
        except Exception as e:
            print(f"检测时发生错误: {e}")
            return None, None, None
    
    def release(self):
        """释放资源"""
        pass  # 传统方法无需特殊释放


def setup_camera_optimized(cap):
    """设置摄像头优化参数 - 多线程优化版本"""
    print("正在优化摄像头设置（多线程模式）...")
    
    # 配置OpenCV多线程优化
    cv2.setUseOptimized(True)
    cv2.setNumThreads(2)  # 为显示线程保留CPU资源
    
    # 基本参数设置
    cap.set(cv2.CAP_PROP_FRAME_WIDTH, 640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 480)
    cap.set(cv2.CAP_PROP_FPS, MAX_FPS)
    
    # 多线程模式优化配置
    optimized_settings = {
        cv2.CAP_PROP_BUFFERSIZE: 1,        # 减少缓冲区，降低延迟
        cv2.CAP_PROP_AUTO_EXPOSURE: 0.25,  # 手动曝光
        cv2.CAP_PROP_EXPOSURE: -6,         # 适中曝光值  
        cv2.CAP_PROP_BRIGHTNESS: 0.6,      # 适中亮度
        cv2.CAP_PROP_CONTRAST: 0.8,        # 高对比度
        cv2.CAP_PROP_SATURATION: 0.7,      # 适中饱和度
        cv2.CAP_PROP_GAIN: 0.5,           # 适中增益
    }
    
    print("应用多线程优化配置...")
    for prop_id, value in optimized_settings.items():
        try:
            cap.set(prop_id, value)
            actual_value = cap.get(prop_id)
            print(f"  设置 {prop_id}: {value} -> 实际: {actual_value:.2f}")
        except Exception as e:
            print(f"  设置 {prop_id} 失败: {e}")
    
    # 验证设置
    W = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    H = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    actual_fps = cap.get(cv2.CAP_PROP_FPS)
    print(f"实际分辨率: {W}×{H}")
    print(f"实际帧率: {actual_fps}")
    
    # 等待摄像头稳定
    print("等待摄像头稳定...")
    for i in range(10):
        ret, frame = cap.read()
        if ret:
            time.sleep(0.1)
    
    print("摄像头多线程优化完成!")
    return cap

def main():
    """主函数 - 传统车辆检测版本"""
    print("=== RK3588 传统车辆检测程序 ===")
    print("CPU: RK3588 (8核 ARM)")
    print("检测方法: 传统计算机视觉 (运动检测 + 轮廓分析)")
    print("线程配置: 摄像头读取线程 + 主检测显示线程")
    
    global stop_threads
    stop_threads.clear()
    
    # 检查摄像头设备
    if not os.path.exists(CAMERA_DEVICE):
        print(f"错误: 摄像头设备不存在: {CAMERA_DEVICE}")
        return
    

    
    # 初始化检测器
    detector = CarDetector()
    print("检测器初始化完成")
    
    # 打开摄像头
    print(f"正在打开摄像头: {CAMERA_DEVICE}")
    cap = cv2.VideoCapture(CAMERA_DEVICE)
    
    if not cap.isOpened():
        print(f"错误: 无法打开摄像头 {CAMERA_DEVICE}")
        detector.release()
        return
    
    # 优化摄像头设置
    cap = setup_camera_optimized(cap)
    
    # 创建线程间通信队列
    frame_queue = ThreadSafeQueue(maxsize=FRAME_QUEUE_SIZE)
    
    # 创建并启动线程
    frame_reader = FrameReader(cap, frame_queue)
    
    frame_reader.start()
    
    print("所有线程已启动!")
    
    # 主线程显示窗口设置
    window_name = "RK3588 传统车辆检测"
    cv2.namedWindow(window_name, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(window_name, 800, 600)
    
    print("=== 传统检测模式操作说明 ===")
    print("基本操作:")
    print("  q - 退出程序")
    print("  s - 保存当前帧")
    print("  p - 暂停/继续显示")
    print("  i - 显示检测信息")
    print("  r - 重置统计")
    
    # 主线程变量
    paused = False
    frame_count = 0
    display_fps = 0
    last_display_time = time.time()
    display_counter = 0
    no_result_count = 0
    total_detections = 0
    detection_fps = 0
    detection_counter = 0
    last_detection_time = time.time()
    
    try:
        while not stop_threads.is_set():
            current_time = time.time()
            
            if not paused:
                # 从帧队列获取摄像头数据
                try:
                    frame_data, success = frame_queue.get(timeout=0.1)
                    
                    if success and frame_data:
                        no_result_count = 0
                        frame, frame_time = frame_data
                        
                        if frame is None:
                            print("警告: 接收到空帧，跳过显示")
                            continue
                        
                        # 执行车辆检测
                        try:
                            detection_start_time = time.time()
                            boxes, classes, scores = detector.detect(frame)
                            detection_end_time = time.time()
                            
                            # 绘制检测结果
                            current_detections = 0
                            if boxes is not None and len(boxes) > 0:
                                current_detections = draw_detections(frame, boxes, scores, classes)
                                total_detections += current_detections
                            
                            # 更新检测FPS
                            detection_counter += 1
                            if current_time - last_detection_time >= 1.0:
                                detection_fps = detection_counter / (current_time - last_detection_time)
                                detection_counter = 0
                                last_detection_time = current_time
                            
                            process_time = detection_end_time - detection_start_time
                            
                        except Exception as e:
                            print(f"检测处理错误: {e}")
                            current_detections = 0
                            process_time = 0
                        
                        # 显示检测结果和性能信息
                        try:
                            info_y = 30
                            info_texts = [
                                f"检测到车辆: {current_detections}",
                                f"总检测数: {total_detections}",
                                f"摄像头FPS: {frame_reader.read_fps:.1f}",
                                f"检测FPS: {detection_fps:.1f}",
                                f"显示FPS: {display_fps:.1f}",
                                f"检测时间: {process_time*1000:.1f}ms",
                                f"帧队列: {frame_queue.qsize()}/{FRAME_QUEUE_SIZE}",
                                "传统检测模式"
                            ]
                            
                            for info_text in info_texts:
                                cv2.putText(frame, info_text, (10, info_y),
                                           cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 255), 2)
                                info_y += 25
                            
                            # 显示线程状态指示器
                            thread_status_y = 30
                            status_text = f"摄像头线程: {'运行' if frame_reader.is_alive() else '停止'}"
                            color = (0, 255, 0) if frame_reader.is_alive() else (0, 0, 255)
                            text_size = cv2.getTextSize(status_text, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)[0]
                            cv2.putText(frame, status_text, 
                                       (frame.shape[1] - text_size[0] - 10, thread_status_y),
                                       cv2.FONT_HERSHEY_SIMPLEX, 0.5, color, 1)
                            
                            cv2.imshow(window_name, frame)
                            
                            # 更新显示FPS
                            display_counter += 1
                            if current_time - last_display_time >= 1.0:
                                display_fps = display_counter / (current_time - last_display_time)
                                display_counter = 0
                                last_display_time = current_time
                            
                            frame_count += 1
                            
                        except Exception as e:
                            print(f"显示性能信息时出错: {e}")
                            # 即使出错也尝试显示基本帧
                            try:
                                cv2.imshow(window_name, frame)
                            except:
                                pass
                            
                    else:
                        no_result_count += 1
                        if no_result_count > 10:  # 10次没有结果，显示等待信息
                            try:
                                # 创建等待画面
                                waiting_frame = np.zeros((480, 640, 3), dtype=np.uint8)
                                wait_text = "等待检测数据..."
                                text_size = cv2.getTextSize(wait_text, cv2.FONT_HERSHEY_SIMPLEX, 1, 2)[0]
                                text_x = (waiting_frame.shape[1] - text_size[0]) // 2
                                text_y = (waiting_frame.shape[0] + text_size[1]) // 2
                                cv2.putText(waiting_frame, wait_text, (text_x, text_y),
                                           cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 255), 2)
                                
                                # 显示线程状态
                                status_text = f"摄像头线程: {'运行' if frame_reader.is_alive() else '停止'}"
                                cv2.putText(waiting_frame, status_text, (10, 50),
                                           cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0) if frame_reader.is_alive() else (0, 0, 255), 2)
                                
                                cv2.imshow(window_name, waiting_frame)
                            except Exception as e:
                                print(f"显示等待画面时出错: {e}")
                
                except Exception as e:
                    print(f"获取检测数据时出错: {e}")
                    import traceback
                    print(f"详细错误信息: {traceback.format_exc()}")
            
            # 处理按键
            try:
                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'):
                    break
                elif key == ord('s') and not paused:
                    if 'frame' in locals() and frame is not None:
                        filename = f"detection_result_{int(time.time())}.jpg"
                        cv2.imwrite(filename, frame)
                        print(f"保存检测结果图像: {filename}")
                elif key == ord('p'):
                    paused = not paused
                    status = "暂停" if paused else "继续"
                    print(f"显示已{status}")
                elif key == ord('i'):
                    # 显示详细线程信息
                    print("\n=== 传统检测性能信息 ===")
                    print(f"摄像头读取FPS: {frame_reader.read_fps:.2f}")
                    print(f"检测FPS: {detection_fps:.2f}")
                    print(f"显示FPS: {display_fps:.2f}")
                    print(f"帧队列大小: {frame_queue.qsize()}/{FRAME_QUEUE_SIZE}")
                    print(f"摄像头线程状态: {'运行' if frame_reader.is_alive() else '停止'}")
                    print(f"总检测车辆数: {total_detections}")
                    print(f"总显示帧数: {frame_count}")
                elif key == ord('r'):
                    # 重置统计
                    frame_count = 0
                    total_detections = 0
                    print("统计数据已重置")
            except Exception as e:
                print(f"处理按键时出错: {e}")
    
    except KeyboardInterrupt:
        print("\n程序被用户中断")
    
    except Exception as e:
        print(f"主线程运行错误: {e}")
        import traceback
        print(f"详细错误信息: {traceback.format_exc()}")
    
    finally:
        # 停止所有线程
        print("正在停止所有线程...")
        stop_threads.set()
        
        # 等待线程结束
        try:
            if 'frame_reader' in locals() and frame_reader.is_alive():
                frame_reader.join(timeout=2)
        except Exception as e:
            print(f"等待线程结束时出错: {e}")
        
        # 清理资源
        print("正在清理资源...")
        try:
            if 'cap' in locals():
                cap.release()
            
            cv2.destroyAllWindows()
            
            if 'detector' in locals():
                detector.release()
        except Exception as e:
            print(f"清理资源时出错: {e}")
        
        print("程序结束")
        print(f"总显示帧数: {frame_count}")
        print(f"总检测车辆数: {total_detections}")
        if display_fps > 0:
            print(f"平均显示FPS: {display_fps:.1f}")
        if detection_fps > 0:
            print(f"平均检测FPS: {detection_fps:.1f}")

if __name__ == '__main__':
    main() 
