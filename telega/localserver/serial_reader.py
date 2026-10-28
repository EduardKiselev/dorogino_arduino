#!/usr/bin/env python3
"""Чтение данных веса с /dev/ttyUSB0 — парсинг формата +XXXXX.X#+YYYYY.YD"""

import sys
import serial
import time

SERIAL_PORT = '/dev/ttyUSB0'
BAUDRATE = 115200
UPDATE_INTERVAL = 0.5  # секунд


def main():
    print(f"Открываю {SERIAL_PORT} на скорости {BAUDRATE}...")
    
    try:
        ser = serial.Serial(
            SERIAL_PORT, 
            BAUDRATE, 
            timeout=1,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            xonxoff=False,
            rtscts=False,
            dsrdtr=False
        )
    except Exception as e:
        print(f"❌ Не удалось открыть {SERIAL_PORT}: {e}")
        sys.exit(1)

    print(f"✅ Подключено к {SERIAL_PORT}")
    print(f"   Формат данных: +XXXXX.X#+YYYYY.YD")
    print(f"   Показываем: второе значение × 1000 (вес)")
    print(f"   Обновление каждые {UPDATE_INTERVAL} сек")
    print(f"   Ctrl+C для выхода\n")
    print("-" * 60)

    time.sleep(0.5)
    ser.reset_input_buffer()
    
    buf = ""
    last_update_time = 0
    
    # Текущие значения
    value1 = None
    value2 = None

    try:
        while True:
            if ser.in_waiting:
                raw_data = ser.read(ser.in_waiting)
                chunk = raw_data.decode('utf-8', errors='ignore')
                buf += chunk
                
                # Ищем полные строки (разделитель \r или \n)
                while '\r' in buf or '\n' in buf:
                    # Ищем первый разделитель
                    cr_pos = buf.find('\r')
                    lf_pos = buf.find('\n')
                    
                    if cr_pos == -1 and lf_pos == -1:
                        break
                    
                    # Берём ближайший разделитель
                    if cr_pos == -1:
                        end_pos = lf_pos
                    elif lf_pos == -1:
                        end_pos = cr_pos
                    else:
                        end_pos = min(cr_pos, lf_pos)
                    
                    line = buf[:end_pos].strip()
                    buf = buf[end_pos + 1:]
                    
                    if not line:
                        continue
                    
                    # Парсим формат: +XXXXX.X#+YYYYY.YD
                    if '#' in line:
                        parts = line.split('#')
                        if len(parts) == 2:
                            try:
                                value1 = float(parts[0])
                                # Убираем букву D в конце второго значения
                                value2_str = parts[1].rstrip('D')
                                value2 = float(value2_str)
                            except ValueError:
                                pass
                
                # Обновляем вывод раз в UPDATE_INTERVAL секунд
                current_time = time.time()
                if value2 is not None:
                    if current_time - last_update_time >= UPDATE_INTERVAL:
                        ts = time.strftime('%H:%M:%S')
                        weight_raw = int(value2 * 1000)
                        print(f"[{ts}] Вес: {weight_raw}")
                        last_update_time = current_time

            time.sleep(0.01)

    except KeyboardInterrupt:
        print(f"\n\n👋 Остановлено")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
