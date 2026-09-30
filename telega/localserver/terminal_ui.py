import sys
import json
import signal
import hashlib
import time
import logging
import os
import serial
from pathlib import Path
from dotenv import load_dotenv
from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QApplication, QWidget, QVBoxLayout, QHBoxLayout,
                               QLabel, QPushButton, QListWidget, QMessageBox,
                               QInputDialog, QLineEdit)

# Настройка логгера
logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
logger = logging.getLogger(__name__)

SERIAL_PORT = '/dev/ttyUSB0'
BAUDRATE = 115200

# Загружаем .env файл
ENV_FILE = Path(__file__).parent / '.env'
load_dotenv(ENV_FILE)


class TerminalUI(QWidget):
    def __init__(self):
        super().__init__()
        self.records = []
        self.current_weight = 0.0
        self.last_fix_weight = 0.0  # Вес при последней фиксации
        self.is_on_target = False
        self.current_tag = ""
        self.current_bin_name = ""
        self.last_rfid_time = 0.0

        self.rfid_mapping = self.load_rfid_mapping()
        self.admin_password_hash = self.load_admin_password()

        self.init_ui()
        self.init_serial()

    # ---------- .ENV ----------
    def load_rfid_mapping(self):
        """Загружает маппинг RFID UID → название бункера из .env файла."""
        mapping = {}
        for key, value in os.environ.items():
            if key.startswith('RFID_'):
                uid = key.replace('RFID_', '')
                mapping[uid] = value
        if mapping:
            logger.debug(f"📋 Загружен маппинг RFID: {mapping}")
        else:
            logger.warning(f"⚠️ Маппинг RFID пуст. Проверь .env файл.")
        return mapping

    def load_admin_password(self):
        """Загружает пароль администратора из .env файла и вычисляет его хэш."""
        password = os.getenv('PASS', 'admin')
        if password == 'admin':
            logger.warning(f"⚠️ Используется пароль по умолчанию. Установи PASS в .env")
        else:
            logger.debug(f"🔐 Пароль администратора загружен из .env")

        return hashlib.sha256(password.encode()).hexdigest()

    # ---------- ИНТЕРФЕЙС ----------
    def init_ui(self):
        self.setStyleSheet("background-color: #1e1e2e; color: #ffffff;")

        main_layout = QVBoxLayout()
        main_layout.setSpacing(15)
        main_layout.setContentsMargins(20, 20, 20, 20)

        # Верхняя панель с кнопкой администратора
        top_layout = QHBoxLayout()
        top_layout.addStretch()

        self.btn_admin = QPushButton("⚙")
        self.btn_admin.setFixedSize(40, 40)
        self.btn_admin.setStyleSheet("""
            QPushButton {
                font-size: 24px;
                background-color: #45475a;
                color: #f9e2af;
                border-radius: 20px;
                border: 2px solid #f9e2af;
            }
            QPushButton:pressed {
                background-color: #585b70;
            }
        """)
        self.btn_admin.clicked.connect(self.admin_login)
        self.btn_admin.setToolTip("Админ-панель")
        top_layout.addWidget(self.btn_admin)

        main_layout.addLayout(top_layout)

        # Первая строка: два веса в одну линию
        weight_row = QHBoxLayout()
        
        # Общий вес
        self.weight_label = QLabel("Общий вес:\n-- кг")
        self.weight_label.setAlignment(Qt.AlignCenter)
        self.weight_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #89dceb;")
        weight_row.addWidget(self.weight_label)
        
        # Вес относительно фиксации
        self.delta_weight_label = QLabel("Относительно фиксации:\n-- кг")
        self.delta_weight_label.setAlignment(Qt.AlignCenter)
        self.delta_weight_label.setStyleSheet("font-size: 32px; color: #f9e2af;")
        weight_row.addWidget(self.delta_weight_label)
        
        main_layout.addLayout(weight_row)

        # Вторая строка: позиция (крупный шрифт как у общего веса)
        self.status_label = QLabel("Позиция: ожидание...")
        self.status_label.setAlignment(Qt.AlignCenter)
        self.status_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #f38ba8;")
        main_layout.addWidget(self.status_label)

        # Список зафиксированных записей
        self.list_widget = QListWidget()
        self.list_widget.setStyleSheet("font-size: 18px; background-color: #313244;")
        main_layout.addWidget(self.list_widget)

        # Кнопки
        btn_layout = QHBoxLayout()

        self.btn_fix = QPushButton("1. Фиксировать вес")
        self.btn_send = QPushButton("2. Отправить на сервер")
        self.btn_reset = QPushButton("3. Сброс")

        for btn in (self.btn_fix, self.btn_send, self.btn_reset):
            btn.setFixedHeight(90)
            btn.setStyleSheet("""
                QPushButton { font-size: 20px; background-color: #45475a;
                              border-radius: 10px; color: white; }
                QPushButton:pressed { background-color: #585b70; }
                QPushButton:disabled { color: #6c7086; background-color: #313244; }
            """)

        self.btn_fix.clicked.connect(self.fix_weight)
        self.btn_send.clicked.connect(self.send_to_server)
        self.btn_reset.clicked.connect(self.reset_records)

        btn_layout.addWidget(self.btn_fix)
        btn_layout.addWidget(self.btn_send)
        btn_layout.addWidget(self.btn_reset)
        main_layout.addLayout(btn_layout)

        self.setLayout(main_layout)
        self.showFullScreen()

        self.update_fix_button()

    # ---------- SERIAL ----------
    def init_serial(self):
        try:
            self.ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=0.1)
            logger.info(f"✅ Serial порт открыт: {SERIAL_PORT}")
        except Exception as e:
            logger.error(f"❌ Ошибка порта: {e}")
            self.ser = None

        # Таймер опроса serial порта
        self.timer = QTimer()
        self.timer.timeout.connect(self.read_serial)
        self.timer.start(100)

        # Таймер проверки устаревания RFID-сигнала (каждые 200 мс)
        self.position_timer = QTimer()
        self.position_timer.timeout.connect(self.check_position_timeout)
        self.position_timer.start(200)

    def read_serial(self):
        """Читает и обрабатывает данные из serial порта."""
        if not self.ser:
            return

        while self.ser.in_waiting:
            line = self.ser.readline().decode('utf-8', errors='ignore').strip()
            if not line:
                continue

            logger.debug(f"📨 Получено от ESP32: {line}")

            try:
                data = json.loads(line)
            except json.JSONDecodeError as e:
                logger.error(f"❌ Ошибка парсинга JSON: {e}, строка: {line}")
                continue

            dtype = data.get("type")
            logger.debug(f"📦 Тип данных: {dtype}")

            if dtype == "weight":
                self.current_weight = data.get("value", 0.0)
                self.update_weight_display()
                logger.debug(f"⚖️ Вес обновлён: {self.current_weight:.1f} кг")

            elif dtype == "rfid":
                uid = data.get("uid", "")
                self.current_tag = uid
                self.last_rfid_time = time.time()

                # Получаем название бункера из маппинга
                self.current_bin_name = self.rfid_mapping.get(uid, f"неизвестный ({uid})")
                logger.debug(f"🏷️ RFID считан: UID={uid}, бункер={self.current_bin_name}")
                self.check_position()

            elif dtype == "position":
                self.is_on_target = data.get("on_target", False)
                logger.debug(f"📍 Позиция от контроллера: on_target={self.is_on_target}")
                self.update_position_ui()

            else:
                logger.warning(f"⚠️ Неизвестный тип данных: {dtype}")

    def update_weight_display(self):
        """Обновляет отображение обоих весов."""
        self.weight_label.setText(f"Общий вес: {self.current_weight:.1f} кг")
        
        delta = self.current_weight - self.last_fix_weight
        sign = "+" if delta >= 0 else ""
        self.delta_weight_label.setText(f"Относительно фиксации: {sign}{delta:.1f} кг")

    def check_position_timeout(self):
        """Периодически проверяет, не устарел ли RFID-сигнал."""
        if self.last_rfid_time > 0 and (time.time() - self.last_rfid_time) > 1.0:
            if self.is_on_target:
                self.is_on_target = False
                logger.debug(f"⏱️ RFID сигнал устарел (>1 сек), позиция сброшена")
                self.update_position_ui()

    # ---------- ЛОГИКА ПОЗИЦИОНИРОВАНИЯ ----------
    def check_position(self):
        """Проверяем позицию: если за последнюю секунду приходил RFID — мы на метке."""
        elapsed = time.time() - self.last_rfid_time
        self.is_on_target = elapsed < 1.0
        logger.debug(f"🎯 Проверка позиции: прошло {elapsed:.2f} сек, на метке: {self.is_on_target}")
        self.update_position_ui()

    def update_position_ui(self):
        """Обновляет UI статуса позиции."""
        if self.is_on_target:
            self.status_label.setText(f"Позиция: ✅ {self.current_bin_name}")
            self.status_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #a6e3a1;")
        else:
            self.status_label.setText("Позиция: ❌ не на метке")
            self.status_label.setStyleSheet("font-size: 48px; font-weight: bold; color: #f38ba8;")
        self.update_fix_button()

    def update_fix_button(self):
        self.btn_fix.setEnabled(self.is_on_target)

    # ---------- АДМИН-ПАНЕЛЬ ----------
    def admin_login(self):
        password, ok = QInputDialog.getText(
            self, "Доступ", "Пароль:", QLineEdit.Password
        )
        if not ok:
            return

        entered_hash = hashlib.sha256(password.encode()).hexdigest()
        if entered_hash == self.admin_password_hash:
            logger.debug(f"✅ Вход в админ-панель успешен")
            self.show_admin_menu()
        else:
            logger.warning(f"❌ Неверный пароль администратора")
            QMessageBox.warning(self, "Ошибка", "Неверный пароль")

    def show_admin_menu(self):
        """Показывает админ-меню в виде диалога с кнопками."""
        dialog = QMessageBox(self)
        dialog.setWindowTitle("Админ-панель")
        dialog.setText("Выберите действие:")
        dialog.setStyleSheet("""
            QMessageBox {
                background-color: #313244;
                color: #ffffff;
            }
            QMessageBox QLabel {
                color: #ffffff;
                font-size: 18px;
            }
            QPushButton {
                font-size: 16px;
                padding: 10px 20px;
                background-color: #45475a;
                color: white;
                border-radius: 5px;
                margin: 5px;
            }
            QPushButton:hover {
                background-color: #585b70;
            }
        """)

        btn_exit = dialog.addButton("🚪 Выйти из программы", QMessageBox.ActionRole)
        btn_cancel = dialog.addButton("Отмена", QMessageBox.RejectRole)

        dialog.exec()

        clicked = dialog.clickedButton()
        if clicked == btn_exit:
            self.close_app()

    def close_app(self):
        if self.ser:
            self.ser.close()
        QApplication.quit()

    # ---------- КНОПКИ ----------
    def fix_weight(self):
        if not self.is_on_target:
            QMessageBox.warning(self, "Ошибка", "Нельзя фиксировать: вы не на метке!")
            logger.warning(f"❌ Попытка фиксации вне позиции")
            return

        tag_name = self.current_bin_name if self.current_bin_name else "бункер"
        record = f"{tag_name} — {self.current_weight:.1f} кг"
        self.records.append({"tag": tag_name, "weight": self.current_weight})
        self.list_widget.addItem(record)
        
        # Сохраняем вес для расчёта дельты
        self.last_fix_weight = self.current_weight
        self.update_weight_display()
        
        logger.info(f"✅ Зафиксировано: {record}")

    def send_to_server(self):
        if not self.records:
            QMessageBox.information(self, "Сервер", "Нет записей для отправки.")
            logger.debug(f"📤 Отправка отменена: нет записей")
            return

        payload = {"action": "send", "records": self.records}
        self.transmit(payload)
        logger.info(f"📤 Отправлено на сервер: {len(self.records)} записей")

        self.records.clear()
        self.list_widget.clear()

    def reset_records(self):
        payload = {"action": "reset", "records": self.records}
        self.transmit(payload)
        logger.info(f"🔄 Сброс: очищено {len(self.records)} записей")

        self.records.clear()
        self.list_widget.clear()
        
        # Сбрасываем вес фиксации
        self.last_fix_weight = 0.0
        self.update_weight_display()

    def transmit(self, payload):
        # TODO: заменить на реальный HTTP/MQTT запрос
        logger.debug(f"🌐 ОТПРАВКА НА СЕРВЕР: {json.dumps(payload, ensure_ascii=False)}")


if __name__ == "__main__":
    app = QApplication(sys.argv)

    signal.signal(signal.SIGINT, signal.SIG_DFL)
    _timer = QTimer()
    _timer.start(200)
    _timer.timeout.connect(lambda: None)

    win = TerminalUI()
    sys.exit(app.exec())