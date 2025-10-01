import os
import glob
import sys

def clean_csv_files(folder_path="out/coverage/"):
    """
    快速清理CSV文件
    """
    csv_files = glob.glob(os.path.join(folder_path, "*"))
    
    for file_path in csv_files:
        # 跳过目录
        if not os.path.isfile(file_path):
            continue
        # 删除空文件
        try:
            size = os.path.getsize(file_path)
        except Exception:
            # 非常规文件，跳过
            continue
        if size == 0:
            os.remove(file_path)
            print(f"remove empty file {file_path}")
            continue
        
        # 处理非空文件
        try:
            with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                lines = f.readlines()
            
            # 分离头部和数据行
            header = []
            data_lines = []
            
            for line in lines:
                line = line.strip()
                if line.startswith("Iter,Cover,SecElapsed"):
                    header = [line]
                elif line.count(',') == 2:
                    parts = line.split(',')
                    # if len(parts) == 3:  # 确保分割后有三个部分
                    #     data_lines.append(line)
                    if len(parts) == 3:
                        # 检查每个字段的类型
                        try:
                            # 第一个字段应该是整数
                            iter_val = int(parts[0])
                            # 第二个字段应该是整数
                            cover_val = int(parts[1])
                            # 第三个字段应该是实数（浮点数）
                            sec_val = float(parts[2])
                            if sec_val <= 0:
                                # 非法时间，跳过
                                raise ValueError("non-positive SecElapsed")
                            # 所有类型检查通过，添加到数据行
                            data_lines.append(line)
                        except ValueError:
                            # 如果类型转换失败，说明格式不正确
                            print(f"remove invalid data format: {line} in {file_path}")
                else:
                    print(f"remove {line} in {file_path}")
            
            # 重新写入文件
            with open(file_path, 'w', encoding='utf-8') as f:
                if header:
                    f.write(header[0] + '\n')
                for line in data_lines:
                    f.write(line + '\n')
                    
        except Exception as e:
            print(f"处理文件失败 {os.path.basename(file_path)}: {e}")
    
    print("文件清理完成！")

if __name__ == "__main__":
    # 允许传参：python3 clean_csv.py <folder>
    folder = sys.argv[1] if len(sys.argv) > 1 else "out/coverage/"
    clean_csv_files(folder)