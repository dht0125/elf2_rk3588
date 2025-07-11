#!/usr/bin/env python3
# -*- coding: utf-8 -*-

from alibabacloud_tea_openapi.client import Client as OpenApiClient
from alibabacloud_tea_openapi import models as open_api_models
from alibabacloud_tea_util import models as util_models
from alibabacloud_openapi_util.client import Client as OpenApiUtilClient

# 阿里云API配置
access_key_id = "xxxxxxxxxxxxxxxxxxxxxxxx"
access_key_secret = "xxxxxxxxxxxxxxxxxxxxxxxx"

def create_client():
    """创建阿里云客户端"""
    config = open_api_models.Config(
        access_key_id=access_key_id,
        access_key_secret=access_key_secret
    )
    # 使用上海区域的endpoint
    config.endpoint = 'iot.cn-shanghai.aliyuncs.com'
    return OpenApiClient(config)

def list_iot_instances():
    """获取IoT实例列表"""
    try:
        print("正在获取IoT实例列表...")
        
        client = create_client()
        
        # 创建API参数
        params = open_api_models.Params(
            action='ListIotInstanceByTags',  # 列出IoT实例
            version='2018-01-20',
            protocol='HTTPS',
            method='POST',
            auth_type='AK',
            style='RPC',
            pathname='/',
            req_body_type='formData',
            body_type='json'
        )
        
        # 构建请求
        queries = {}
        request = open_api_models.OpenApiRequest(
            query=OpenApiUtilClient.query(queries)
        )
        runtime = util_models.RuntimeOptions()
        
        # 发送请求
        response = client.call_api(params, request, runtime)
        result = response.get('body')
        
        print(f"API响应: {result}")
        
        if result and 'Data' in result:
            instances = result['Data']
            print(f"\n找到 {len(instances)} 个IoT实例:")
            for i, instance in enumerate(instances):
                print(f"{i+1}. 实例ID: {instance.get('IotInstanceId', 'N/A')}")
                print(f"   实例名称: {instance.get('InstanceName', 'N/A')}")
                print(f"   区域: {instance.get('RegionId', 'N/A')}")
                print(f"   状态: {instance.get('Status', 'N/A')}")
                print("---")
        else:
            print("没有找到IoT实例或响应格式不正确")
            
    except Exception as e:
        print(f"获取实例列表失败: {e}")

def try_public_instance():
    """尝试获取公共实例信息"""
    try:
        print("\n尝试获取公共实例信息...")
        
        client = create_client()
        
        # 尝试不同的API来获取实例信息
        params = open_api_models.Params(
            action='QueryDeviceGroupInfo',  # 查询设备组信息
            version='2018-01-20',
            protocol='HTTPS',
            method='POST',
            auth_type='AK',
            style='RPC',
            pathname='/',
            req_body_type='formData',
            body_type='json'
        )
        
        queries = {}
        queries['GroupId'] = 'test'  # 随便一个组ID
        
        request = open_api_models.OpenApiRequest(
            query=OpenApiUtilClient.query(queries)
        )
        runtime = util_models.RuntimeOptions()
        
        response = client.call_api(params, request, runtime)
        result = response.get('body')
        
        print(f"公共实例测试响应: {result}")
        
    except Exception as e:
        print(f"公共实例测试失败: {e}")
        # 如果错误信息中包含实例ID，我们可以从中提取
        error_str = str(e)
        if 'iot-' in error_str:
            print("从错误信息中可能包含实例ID信息")

if __name__ == '__main__':
    print("阿里云IoT实例ID查找工具")
    print("=" * 40)
    
    list_iot_instances()
    try_public_instance()
    
    print("\n如果上述方法都没有找到实例ID，请:")
    print("1. 登录阿里云控制台")
    print("2. 进入物联网平台控制台")
    print("3. 在实例管理页面查看你的实例ID")
    print("4. 公共实例的ID通常格式为: iot-06z00xxxxx") 