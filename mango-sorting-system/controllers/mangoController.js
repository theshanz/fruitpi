const db = require("../config/db");

// Save Mango Data
exports.addMango = (req, res) => {
    const { status } = req.body;

    if (!status) {
        return res.status(400).json({
            message: "Status is required"
        });
    }

    const sql = "INSERT INTO mangoes (status) VALUES (?)";

    db.query(sql, [status], (err) => {
        if (err) {
            return res.status(500).json(err);
        }

        res.json({
            message: "Mango data saved successfully!"
        });
    });
};

// Dashboard Data
exports.getDashboard = (req, res) => {

    const sql = `
        SELECT
            COUNT(*) AS total,
            SUM(status='Good') AS good,
            SUM(status='Bad') AS bad
        FROM mangoes
    `;

    db.query(sql, (err, result) => {

        if (err) {
            return res.status(500).json(err);
        }

        const total = result[0].total || 0;
        const good = result[0].good || 0;
        const bad = result[0].bad || 0;

        const goodPercentage = total ? ((good / total) * 100).toFixed(2) : 0;
        const badPercentage = total ? ((bad / total) * 100).toFixed(2) : 0;

        res.json({
            total,
            good,
            bad,
            goodPercentage,
            badPercentage
        });

    });

};

exports.getDailyReport = (req, res) => {

    const sql = `
        SELECT
            DATE(created_at) AS date,
            COUNT(*) AS total,
            SUM(status='Good') AS good,
            SUM(status='Bad') AS bad
        FROM mangoes
        GROUP BY DATE(created_at)
        ORDER BY DATE(created_at) DESC;
    `;

    db.query(sql, (err, results) => {

        if (err) {
            return res.status(500).json(err);
        }

        const report = results.map(row => ({
            date: row.date,
            total: row.total,
            good: row.good || 0,
            bad: row.bad || 0,
            goodPercentage: ((row.good / row.total) * 100).toFixed(2),
            badPercentage: ((row.bad / row.total) * 100).toFixed(2)
        }));

        res.json(report);

    });

};

exports.getWeeklyReport = (req, res) => {

    const sql = `
        SELECT
            YEAR(created_at) AS year,
            WEEK(created_at) AS week,
            COUNT(*) AS total,
            SUM(status='Good') AS good,
            SUM(status='Bad') AS bad
        FROM mangoes
        GROUP BY YEAR(created_at), WEEK(created_at)
        ORDER BY year DESC, week DESC;
    `;


    db.query(sql, (err, results) => {

        if (err) {
            return res.status(500).json(err);
        }


        const report = results.map(row => ({

            week: `${row.year}-W${row.week}`,

            total: row.total,

            good: row.good || 0,

            bad: row.bad || 0,

            goodPercentage:
                ((row.good / row.total) * 100).toFixed(2),

            badPercentage:
                ((row.bad / row.total) * 100).toFixed(2)

        }));


        res.json(report);

    });

};