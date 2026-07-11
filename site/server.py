from flask import Flask, request, jsonify, send_from_directory
from PIL import Image

app = Flask(__name__)

@app.route('/')
def home():
    # Serve index.html from current folder
    return send_from_directory('.', 'index.html')

@app.route('/analyze', methods=['POST'])
def analyze():
    file = request.files['image']
    img = Image.open(file)
    status = "Good" if img.width > 500 else "Bad"
    confidence = 90.0
    return jsonify({"status": status, "confidence": confidence})

if __name__ == '__main__':
    app.run(debug=True)
