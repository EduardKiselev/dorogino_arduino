import serial
import serial.tools.list_ports
import time

def find_esp32_port():
    """Автоматически ищем порт, к которому подключена ESP32."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        # ESP32 обычно определяется как CP210x, CH340 или просто USB Serial
        if 'CP210' in port.description or 'CH340' in port.description or 'USB' in port.description:
            return port.device
    # Если не нашли — возвращаем первый попавшийся
    return ports[0].device if ports else None

def main():
    port = find_esp32_port()
    
    if not port:
        print("❌ ESP32 не найдена. Проверь подключение.")
        return

    print(f"🔌 Подключение к порту: {port}")
    
    # Открываем порт на той же скорости, что и в ESP32 (115200)
    ser = serial.Serial(port, 115200, timeout=1)
    time.sleep(2)  # Ждём, пока порт инициализируется
    
    print("👂 Ожидание меток...\n")

    try:
        while True:
            # Читаем строку из порта
            if ser.in_waiting > 0:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
                
                if not line:
                    continue
                
                # Ищем строку с UID метки
                if "UID метки:" in line:
                    # Извлекаем сам UID (всё после "UID метки:")
                    uid = line.split("UID метки:")[-1].strip()
                    timestamp = time.strftime("%H:%M:%S")
                    
                    print(f"[{timestamp}] ✅ Считана метка: {uid}")
                    
                else:
                    # Выводим остальные служебные сообщения от ESP32
                    print(f"  (ESP32): {line}")

    except KeyboardInterrupt:
        print("\n🛑 Сервис остановлен.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()