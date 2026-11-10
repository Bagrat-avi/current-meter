import sqlite3
import csv
import io
import math
from pathlib import Path

from flask import Flask, request, jsonify, render_template, Response

app = Flask(__name__)

DB_PATH = Path(__file__).with_name("measurements.db")


def init_db():
    with sqlite3.connect(DB_PATH) as con:
        con.execute("""
            CREATE TABLE IF NOT EXISTS measurements (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_time_ms INTEGER NOT NULL,
                current_uA REAL NOT NULL,
                received_at TEXT DEFAULT CURRENT_TIMESTAMP
            )
        """)


@app.route("/")
def home():
    return render_template("index.html")


@app.route("/health")
def health():
    return jsonify({"status": "ok"})


@app.route("/measurement", methods=["POST"])
def receive_measurement():
    data = request.get_json(silent=True)

    if not isinstance(data, dict):
        return jsonify({"error": "Invalid JSON"}), 400

    samples = data.get("samples")

    if not isinstance(samples, list) or not samples:
        return jsonify({"error": "No samples"}), 400

    if len(samples) > 1000:
        return jsonify({"error": "Too many samples"}), 400

    rows = []

    try:
        for sample in samples:
            if not isinstance(sample, dict):
                raise ValueError()

            time_ms = int(sample["time_ms"])
            current = float(sample["current_uA"])

            if time_ms < 0 or not math.isfinite(current):
                raise ValueError()

            rows.append((time_ms, current))

    except (KeyError, TypeError, ValueError, OverflowError):
        return jsonify({"error": "Invalid sample format"}), 400

    with sqlite3.connect(DB_PATH, timeout=10) as con:
        con.executemany("""
            INSERT INTO measurements (device_time_ms, current_uA)
            VALUES (?, ?)
        """, rows)

    print("Получено измерений:", len(rows))

    return jsonify({
        "status": "ok",
        "saved": len(rows)
    })


@app.route("/api/data")
def get_data():
    limit = request.args.get("limit", default=500, type=int)
    limit = max(1, min(limit, 5000))

    with sqlite3.connect(DB_PATH) as con:
        con.row_factory = sqlite3.Row

        rows = con.execute("""
            SELECT id, device_time_ms, current_uA, received_at
            FROM measurements
            ORDER BY id DESC
            LIMIT ?
        """, (limit,)).fetchall()

    return jsonify([dict(row) for row in reversed(rows)])


@app.route("/api/latest")
def get_latest():
    with sqlite3.connect(DB_PATH) as con:
        con.row_factory = sqlite3.Row

        row = con.execute("""
            SELECT id, device_time_ms, current_uA, received_at
            FROM measurements
            ORDER BY id DESC
            LIMIT 1
        """).fetchone()

    if row is None:
        return jsonify({"status": "empty"})

    return jsonify(dict(row))


@app.route("/api/export.csv")
def export_csv():
    with sqlite3.connect(DB_PATH) as con:
        con.row_factory = sqlite3.Row

        rows = con.execute("""
            SELECT id, device_time_ms, current_uA, received_at
            FROM measurements
            ORDER BY id
        """).fetchall()

    output = io.StringIO()
    writer = csv.writer(output)

    writer.writerow([
        "ID",
        "Time (ms)",
        "Current (uA)",
        "Received at"
    ])

    for row in rows:
        writer.writerow([
            row["id"],
            row["device_time_ms"],
            row["current_uA"],
            row["received_at"]
        ])

    response = Response(
        "\ufeff" + output.getvalue(),
        content_type="text/csv; charset=utf-8"
    )

    response.headers["Content-Disposition"] = (
        "attachment; filename=measurements.csv"
    )

    return response


if __name__ == "__main__":
    init_db()
    app.run(host="0.0.0.0", port=5000, debug=False)
