#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
RKNNLite环境测试脚本
用于验证RKNNLite是否正确安装和配置
"""

import sys
import os
import numpy as np

def test_rknnlite_import():
    """测试RKNNLite导入"""
    print("测试1: 导入RKNNLite模块...")
    try:
        from rknnlite.api import RKNNLite
        print("✓ RKNNLite导入成功")
        return True
    except ImportError as e:
        print(f"✗ RKNNLite导入失败: {e}")
        return False

def test_opencv_import():
    """测试OpenCV导入"""
    print("\n测试2: 导入OpenCV模块...")
    try:
        import cv2
        print(f"✓ OpenCV导入成功，版本: {cv2.__version__}")
        return True
    except ImportError as e:
        print(f"✗ OpenCV导入失败: {e}")
        return False

def test_numpy_import():
    """测试NumPy导入"""
    print("\n测试3: 导入NumPy模块...")
    try:
        import numpy as np
        print(f"✓ NumPy导入成功，版本: {np.__version__}")
        return True
    except ImportError as e:
        print(f"✗ NumPy导入失败: {e}")
        return False

def test_model_file():
    """测试模型文件是否存在"""
    print("\n测试4: 检查模型文件...")
    model_path = "zhongji.rknn"
    if os.path.exists(model_path):
        file_size = os.path.getsize(model_path) / (1024 * 1024)  # MB
        print(f"✓ 模型文件存在: {model_path} ({file_size:.2f} MB)")
        return True
    else:
        print(f"✗ 模型文件不存在: {model_path}")
        return False

def test_camera_device():
    """测试摄像头设备"""
    print("\n测试5: 检查摄像头设备...")
    camera_device = "/dev/video21"
    if os.path.exists(camera_device):
        print(f"✓ 摄像头设备存在: {camera_device}")
        return True
    else:
        print(f"✗ 摄像头设备不存在: {camera_device}")
        
        # 列出所有可用的摄像头设备
        video_devices = []
        for i in range(30):  # 检查video0到video29
            device = f"/dev/video{i}"
            if os.path.exists(device):
                video_devices.append(device)
        
        if video_devices:
            print("  可用的摄像头设备:")
            for device in video_devices:
                print(f"    - {device}")
        else:
            print("  未找到任何摄像头设备")
        
        return False

def test_rknnlite_model_loading():
    """测试RKNNLite模型加载"""
    print("\n测试6: 测试RKNNLite模型加载...")
    
    if not os.path.exists("zhongji.rknn"):
        print("✗ 跳过模型加载测试: 模型文件不存在")
        return False
    
    try:
        from rknnlite.api import RKNNLite
        
        # 创建RKNNLite对象
        rknn = RKNNLite(verbose=False)
        
        # 加载模型
        ret = rknn.load_rknn("zhongji.rknn")
        if ret != 0:
            print(f"✗ 模型加载失败: {ret}")
            return False
        
        # 初始化运行时环境
        ret = rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_AUTO)
        if ret != 0:
            print(f"✗ 运行时环境初始化失败: {ret}")
            rknn.release()
            return False
        
        print("✓ RKNNLite模型加载和初始化成功")
        
        # 释放资源
        rknn.release()
        return True
        
    except Exception as e:
        print(f"✗ RKNNLite模型加载测试失败: {e}")
        return False

def test_opencv_camera():
    """测试OpenCV摄像头访问"""
    print("\n测试7: 测试OpenCV摄像头访问...")
    
    try:
        import cv2
        
        # 尝试打开摄像头
        cap = cv2.VideoCapture("/dev/video21")
        
        if not cap.isOpened():
            print("✗ 无法打开摄像头 /dev/video21")
            
            # 尝试其他摄像头
            for i in range(5):
                test_cap = cv2.VideoCapture(i)
                if test_cap.isOpened():
                    print(f"  发现可用摄像头: /dev/video{i}")
                    test_cap.release()
                    break
                test_cap.release()
            
            return False
        
        # 尝试读取一帧
        ret, frame = cap.read()
        if ret:
            print(f"✓ 摄像头访问成功，帧尺寸: {frame.shape}")
            cap.release()
            return True
        else:
            print("✗ 无法从摄像头读取帧")
            cap.release()
            return False
            
    except Exception as e:
        print(f"✗ OpenCV摄像头测试失败: {e}")
        return False

def main():
    """主函数"""
    print("=== RKNNLite环境测试 ===")
    print("此脚本将测试RKNNLite环境是否正确配置\n")
    
    tests = [
        test_rknnlite_import,
        test_opencv_import,
        test_numpy_import,
        test_model_file,
        test_camera_device,
        test_rknnlite_model_loading,
        test_opencv_camera
    ]
    
    passed = 0
    total = len(tests)
    
    for test_func in tests:
        if test_func():
            passed += 1
    
    print(f"\n=== 测试结果 ===")
    print(f"通过: {passed}/{total}")
    
    if passed == total:
        print("✓ 所有测试通过！环境配置正确，可以运行车辆检测程序。")
        return 0
    else:
        print("✗ 部分测试失败，请检查以上错误信息并解决相关问题。")
        return 1

if __name__ == "__main__":
    sys.exit(main()) 