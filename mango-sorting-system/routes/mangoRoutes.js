const express = require("express");
const router = express.Router();

const {
    addMango,
    getDashboard,
    getDailyReport,
    getWeeklyReport
} = require("../controllers/mangoController");

router.post("/mango", addMango);
router.get("/dashboard", getDashboard);
router.get("/daily-report", getDailyReport);
router.get("/weekly-report", getWeeklyReport);

module.exports = router;