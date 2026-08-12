import re
import os

def parse_array(filepath, var_name, data_type, out_file):
    print(f"Parsing {filepath} for {var_name}...")
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Match the content between curly braces
    match = re.search(r'\{\s*(.*?)\s*\};', content, re.DOTALL)
    if not match:
        print(f"Could not find array data in {filepath}")
        return
        
    data_str = match.group(1)
    # Remove comments if any
    data_str = re.sub(r'/\*.*?\*/', '', data_str, flags=re.DOTALL)
    
    # Split by comma
    items = data_str.replace('\n', ' ').split(',')
    
    print(f"Writing to {out_file}...")
    with open(out_file, 'wb') as f:
        for item in items:
            item = item.strip()
            if not item: continue
            
            # handle hex or decimal
            base = 16 if item.startswith('0x') else 10
            try:
                val = int(item, base)
                if data_type == 'char':
                    f.write(val.to_bytes(1, byteorder='little', signed=False))
                elif data_type == 'short':
                    f.write(val.to_bytes(2, byteorder='little', signed=False))
                elif data_type == 'int':
                    f.write(val.to_bytes(4, byteorder='little', signed=False))
            except Exception as e:
                pass
    print(f"Done {out_file}")

def main():
    base_dir = r"C:\Users\JorKsX\Desktop\Tanmatsu custom\components\cmu_us_kal"
    
    parse_array(os.path.join(base_dir, 'cmu_us_kal_lpc.c'), 'cmu_us_kal_lpc', 'short', 'kal_lpc.bin')
    parse_array(os.path.join(base_dir, 'cmu_us_kal_res.c'), 'cmu_us_kal_res', 'char', 'kal_res.bin')
    parse_array(os.path.join(base_dir, 'cmu_us_kal_residx.c'), 'cmu_us_kal_resi', 'int', 'kal_resi.bin')
    parse_array(os.path.join(base_dir, 'cmu_us_kal_ressize.c'), 'cmu_us_kal_ressize', 'char', 'kal_ressize.bin')

if __name__ == '__main__':
    main()
