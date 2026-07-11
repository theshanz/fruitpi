from flask import Flask, request, jsonify
from PIL import Image

app = Flask(__name__)

@app.route('/analyze', methods=['POST'])
def analyze():
    file = request.files['image']
    img = Image.open(file)

    # 🔸 Replace with your ML model logic
    # Example: dummy classification
    status = "Good" if img.width > 500 else "Bad"
    confidence = 90.0

    return jsonify({"status": status, "confidence": confidence})

if __name__ == '__main__':
    app.run(debug=True)
