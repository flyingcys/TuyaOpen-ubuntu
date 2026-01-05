#!/usr/bin/env python3
# coding=utf-8
# 参数说明：
# $1 - params path: $1/build_param.[cmake/config/json]
# $2 - user cmd: build/clean/...

import os
import sys
import json
import subprocess
import shutil
import errno
import tempfile

def clean(root):
    shutil.rmtree("build", ignore_errors=True)
    pass

def parser_para_file(json_file):
    if not os.path.isfile(json_file):
        print(f"Error: File not found [{json_file}].")
        return {}
    try:
        with open(json_file, 'r', encoding='utf-8') as f:
            json_data = json.load(f)
    except Exception as e:
        print(f"JSON parsing error: [{str(e)}].")
        return {}
    return json_data


def check_dependency_changes(build_param_path, build_dir="build"):
    """
    检查应用库文件是否比当前可执行文件更新
    如果是，则需要强制重新构建以避免缓存问题
    """
    try:
        build_param_file = os.path.join(build_param_path, "build_param.json")
        param_data = parser_para_file(build_param_file)
        if not param_data:
            return False
        
        project_name = param_data.get('CONFIG_PROJECT_NAME', '')
        libs_dir = param_data.get('OPEN_LIBS_DIR', '')
        
        if not project_name or not libs_dir:
            return False
            
        # 检查关键库文件
        key_libs = ['libtuyaapp.a', 'libtuyaos.a']
        current_exe = os.path.join(build_dir, project_name)
        
        if not os.path.exists(current_exe):
            return True  # 可执行文件不存在，需要构建
            
        exe_mtime = os.path.getmtime(current_exe)
        
        for lib_name in key_libs:
            lib_path = os.path.join(libs_dir, lib_name)
            if os.path.exists(lib_path):
                lib_mtime = os.path.getmtime(lib_path)
                if lib_mtime > exe_mtime:
                    print(f"Detected library file {lib_name} has been updated, rebuilding to avoid cache issues")
                    return True
                    
        return False
        
    except Exception as e:
        print(f"Error occurred during dependency check: {e}")
        return True  # Rebuild on error

def safe_copy(src, dst):
    dst_dir = os.path.dirname(dst)
    os.makedirs(dst_dir, exist_ok=True)
    fd, tmp_path = tempfile.mkstemp(prefix=os.path.basename(dst) + ".tmp.", dir=dst_dir)
    os.close(fd)
    try:
        shutil.copyfile(src, tmp_path)
        os.replace(tmp_path, dst)
        shutil.copymode(src, dst)
    except OSError as e:
        if e.errno == errno.ETXTBSY:
            fallback = dst + ".new"
            try:
                os.replace(tmp_path, fallback)
                shutil.copymode(src, fallback)
                print(f"Warning: {dst} is busy, wrote {fallback} instead. Stop the running process and replace manually.")
                return
            except OSError:
                pass
        raise
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass

def main():

    if len(sys.argv) < 2:
        print(f"Error: At least 2 parameters are needed {sys.argv}.")
    build_param_path = sys.argv[1]
    user_cmd = sys.argv[2]
    root = os.path.dirname(os.path.abspath(__file__))
    if "clean" == user_cmd:
        clean(root)
        sys.exit(0)

    build_param_file = os.path.join(build_param_path, "build_param.json")
    param_data = parser_para_file(build_param_file)
    if not len(param_data):
        sys.exit(1)

    if not os.path.isfile("CMakeLists.txt"):
        print("Error: CMakeLists.txt not found in the current directory.")
        sys.exit(1)

    # Check dependency changes, clean build directory if needed
    if check_dependency_changes(build_param_path):
        print("Detected application library updates, cleaning platform build cache...")
        clean(root)

    try:
        os.makedirs("build", exist_ok=True)
        
        cmake_command = [
            "cmake", "-S", ".", "-B", "build", "-G", "Ninja",
            f"-DBUILD_PARAM_PATH={build_param_path}"
        ]

        subprocess.run(
            cmake_command,
            check=True,
        )
        subprocess.run(["cmake", "--build", "build"], check=True)
    except subprocess.CalledProcessError as e:
        print(f"CMake build process failed: {e}")
        sys.exit(1)

    source_file = os.path.join(os.getcwd(), "build", param_data['CONFIG_PROJECT_NAME'])
    dst_dir = param_data["BIN_OUTPUT_DIR"]

    if not os.path.exists(dst_dir):
        os.makedirs(dst_dir, exist_ok=True)
        
    destination_path = os.path.join(dst_dir, param_data['CONFIG_PROJECT_NAME'] + "_QIO_" + param_data['CONFIG_PROJECT_VERSION'] + ".bin")
    safe_copy(source_file, destination_path)

    destination_path = os.path.join(dst_dir, param_data['CONFIG_PROJECT_NAME'] + "_" + param_data['CONFIG_PROJECT_VERSION'] + ".elf")
    safe_copy(source_file, destination_path)
    
    sys.exit(0)

if __name__ == "__main__":
    main()
