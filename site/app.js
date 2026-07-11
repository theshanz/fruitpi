document.getElementById('form').addEventListener('submit', async (e) => {
  e.preventDefault();
  
  const file = document.getElementById('file-input').files[0];
  if (!file) return;

  const formData = new FormData();
  formData.append('image', file);

  // Send to backend API
 const response = await fetch('http://127.0.0.1:5501/analyze', {
    method: 'POST',
    body: formData
});
  const result = await response.json();
  document.getElementById('output').innerHTML = `
    <h3>Result: ${result.status}</h3>
    <p>Confidence: ${result.confidence}%</p>
  `;
});
