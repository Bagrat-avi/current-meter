
const currentValue = document.getElementById("currentValue");
const sampleCount = document.getElementById("sampleCount");
const statusElement = document.getElementById("status");
const tableBody = document.getElementById("measurementsBody");
const canvas = document.getElementById("chart");

function drawChart(data) {
    const width = canvas.clientWidth;
    const height = 320;

    const ratio = window.devicePixelRatio || 1;

    canvas.width = width * ratio;
    canvas.height = height * ratio;

    const ctx = canvas.getContext("2d");
    ctx.scale(ratio, ratio);

    ctx.clearRect(0, 0, width, height);

    if (data.length === 0) {
        ctx.fillStyle = "#777";
        ctx.font = "15px Arial";
        ctx.fillText("Ожидаем измерения от ESP32", 20, 35);
        return;
    }

    const values = data.map(x => Number(x.current_uA));

    let min = Math.min(...values);
    let max = Math.max(...values);

    if (min === max) {
        min -= 1;
        max += 1;
    }

    const left = 55;
    const right = width - 15;
    const top = 15;
    const bottom = height - 35;

    const plotWidth = right - left;
    const plotHeight = bottom - top;

    ctx.font = "11px Arial";
    ctx.strokeStyle = "#ddd";
    ctx.fillStyle = "#555";

    for (let i = 0; i <= 4; i++) {
        const y = top + i * plotHeight / 4;

        ctx.beginPath();
        ctx.moveTo(left, y);
        ctx.lineTo(right, y);
        ctx.stroke();

        const value = max - i * (max - min) / 4;
        ctx.fillText(value.toFixed(2), 3, y + 4);
    }

    ctx.fillText("Время →", left, height - 8);

    const times = data.map(x => Number(x.device_time_ms));

    const increasing = times.every(
        (v, i) => i === 0 || v >= times[i - 1]
    );

    const useTime = increasing && times[times.length - 1] > times[0];

    const xValues = useTime
        ? times.map(v => v - times[0])
        : data.map((v, i) => i);

    const maxX = xValues[xValues.length - 1] || 1;

    ctx.strokeStyle = "#159447";
    ctx.lineWidth = 2;
    ctx.beginPath();

    values.forEach((value, i) => {
        const x = left + xValues[i] / maxX * plotWidth;
        const y = bottom - (value - min) / (max - min) * plotHeight;

        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
    });

    ctx.stroke();
}

async function updateData() {
    try {
        const response = await fetch("/api/data?limit=500");

        if (!response.ok) {
            throw new Error("Ошибка получения данных");
        }

        const data = await response.json();

        sampleCount.textContent = data.length;

        currentValue.textContent = data.length
            ? Number(data[data.length - 1].current_uA).toFixed(3)
            : "—";

        tableBody.replaceChildren();

        data.slice(-20).reverse().forEach(item => {
            const row = document.createElement("tr");

            [
                item.id,
                item.device_time_ms,
                Number(item.current_uA).toFixed(3),
                item.received_at
            ].forEach(value => {
                const cell = document.createElement("td");
                cell.textContent = value;
                row.appendChild(cell);
            });

            tableBody.appendChild(row);
        });

        drawChart(data);
        statusElement.textContent = "Связь с сервером установлена";

    } catch (error) {
        statusElement.textContent = "Нет связи: " + error.message;
    }
}

updateData();
setInterval(updateData, 1000);
window.addEventListener("resize", updateData);
