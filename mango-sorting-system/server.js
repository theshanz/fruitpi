const express = require("express");
const cors = require("cors");

const app = express();
const PORT = 3000;

require("./config/db");

const mangoRoutes = require("./routes/mangoRoutes");

app.use(cors());
app.use(express.json());
app.use(express.static("public"));

app.use("/api", mangoRoutes);

app.listen(PORT, () => {
    console.log(`🚀 Server running on http://localhost:${PORT}`);
});