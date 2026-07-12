const mysql = require("mysql2");

const db = mysql.createConnection({
    host: "localhost",
    user: "admin",
    password: "admin123",
    database: "mango_sorting"
});

db.connect((err) => {
    if (err) {
        console.error("Database connection failed:", err);
        return;
    }

    console.log("✅ MySQL Connected");
});

module.exports = db;