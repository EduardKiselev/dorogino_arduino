from flask import Flask, request, jsonify
import json
from datetime import datetime

app = Flask(__name__)

def print_formatted_json(data, endpoint):
    """Красивый вывод JSON в консоль"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"\n{'='*80}")
    print(f"[{timestamp}] Received on {endpoint}")
    print(f"{'='*80}")
    print(json.dumps(data, indent=2, ensure_ascii=False))
    print(f"{'='*80}\n")

@app.route('/api/heartbeat', methods=['POST'])
def heartbeat():
    data = request.get_json()
    print_formatted_json(data, '/api/heartbeat')
    return jsonify({"status": "ok"}), 200

@app.route('/api/data', methods=['POST'])
def data():
    data = request.get_json()
    print_formatted_json(data, '/api/data')
    return jsonify({"status": "ok"}), 200

@app.route('/', methods=['GET'])
def index():
    return jsonify({
        "service": "ESP32 Echo Server",
        "endpoints": [
            "POST /api/heartbeat",
            "POST /api/data"
        ]
    })

if __name__ == '__main__':
    print("Starting ESP32 Echo Server on port 5000...")
    app.run(host='0.0.0.0', port=5000, debug=False)