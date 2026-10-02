from flask import Flask, request, jsonify
import logging
from datetime import datetime

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
logger = logging.getLogger(__name__)

app = Flask(__name__)


@app.route('/api/records', methods=['POST'])
def records():
    """Эхо-сервер для /api/records"""
    data = request.get_json()
    
    logger.info("=" * 60)
    logger.info(f"📥 POST /api/records")
    logger.info(f"📊 Получено записей: {len(data.get('records', []))}")
    logger.info(f"🆔 Device ID: {data.get('device_id')}")
    logger.info(f"📤 Sent at: {data.get('sent_at')}")
    logger.info(f"🎬 Action: {data.get('action')}")
    
    if data.get('records'):
        for i, record in enumerate(data['records'], 1):
            logger.info(f"  {i}. {record.get('tag')} — {record.get('weight')} кг @ {record.get('timestamp')}")
    
    logger.info("=" * 60)
    
    # Возвращаем эхо
    return jsonify({
        "status": "ok",
        "message": "Echo response",
        "received": data
    }), 200


@app.route('/api/heartbeat', methods=['POST'])
def heartbeat():
    """Эхо-сервер для /api/heartbeat"""
    data = request.get_json()
    
    logger.info(f"💓 Heartbeat от {data.get('device_id')}")
    logger.info(f"   CPU: {data.get('health', {}).get('cpu_percent')}%")
    logger.info(f"   RAM: {data.get('health', {}).get('memory_percent')}%")
    logger.info(f"   Uptime: {data.get('health', {}).get('uptime_seconds')}с")
    logger.info(f"   Serial: {data.get('health', {}).get('serial_connected')}")
    logger.info(f"   On target: {data.get('health', {}).get('is_on_target')}")
    logger.info(f"   Weight: {data.get('health', {}).get('current_weight')} кг")
    
    return jsonify({
        "status": "ok",
        "message": "Heartbeat received"
    }), 200


@app.route('/api/<path:path>', methods=['POST', 'GET'])
def catch_all(path):
    """Обработчик для любых других /api/* путей"""
    logger.warning(f"⚠️ Неизвестный endpoint: /api/{path}")
    
    if request.method == 'POST':
        data = request.get_json(silent=True)
        logger.info(f"📥 POST /api/{path}")
        if data:
            logger.info(f"📊 Data: {data}")
        
        return jsonify({
            "status": "ok",
            "message": f"Echo: /api/{path}",
            "received": data
        }), 200
    
    return jsonify({
        "status": "ok",
        "message": f"Echo server running at /api/{path}"
    }), 200


if __name__ == '__main__':
    logger.info("🚀 Запуск эхо-сервера на http://127.0.0.1:5555")
    logger.info("📡 Endpoints:")
    logger.info("   POST /api/records")
    logger.info("   POST /api/heartbeat")
    logger.info("   ANY  /api/*")
    
    app.run(host='127.0.0.1', port=5555, debug=False)