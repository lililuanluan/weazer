import random

def generate_random_numbers(n):
    """生成 n 个 [0, 1) 范围内的随机数"""
    return [random.random() for _ in range(n)]

if __name__ == "__main__":
    num_samples = 5000  # 生成随机数的数量
    random_numbers = generate_random_numbers(num_samples)
    
    print("#ifndef MYRAND")
    print("#define MYRAND\n")
    print("double nums[] = {")
    for number in random_numbers:
        print(number, end=',')
    print("};")
    print("#endif")