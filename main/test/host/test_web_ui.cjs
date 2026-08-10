#!/usr/bin/env node

const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const path = require('node:path');
const { chromium } = require('playwright-core');

const repo = path.resolve(__dirname, '..', '..', '..');
const htmlPath = path.join(repo, 'main', 'index.html');
const outputDir = path.join(repo, 'build');
const chrome = process.argv[2] ||
    'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const html = fs.readFileSync(htmlPath, 'utf8')
    .replace('CHANNEL_COUNT_PLACEHOLDER', '8');

async function main() {
    fs.mkdirSync(outputDir, { recursive: true });
    const server = http.createServer((request, response) => {
        response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        response.end(html);
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));

    let browser;
    try {
    browser = await chromium.launch({ executablePath: chrome, headless: true });
    const page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
    page.setDefaultTimeout(5000);
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(error.message));
    await page.route(/^https:\/\//, (route) => route.abort());
    await page.addInitScript(() => {
        class MockWebSocket {
            static OPEN = 1;
            constructor() { this.readyState = MockWebSocket.OPEN; }
            send() {}
            close() {}
        }
        window.WebSocket = MockWebSocket;
    });
    await page.goto(`http://127.0.0.1:${server.address().port}/`, {
        waitUntil: 'domcontentloaded',
    });

    assert.equal(await page.locator('#extruder option').count(), 9);
    assert.equal(await page.locator('#current-extruder').textContent(),
                 '等待打印机状态');

    await page.evaluate(() => renderPrinterOperationStatus({
        ams_status_info: {
            raw: 260, main: 1, sub: 4, known: true,
            label_zh: '回抽当前耗材，Top-AMS 正在退线',
        },
        printer_fault: { active: false },
        printer_status_ready: true,
        channel_confirmed: true,
        channel_confirmation_required: false,
        hw_switch: 1,
        current_channel: 2,
        nozzle_actual_c: 218.4,
        motor_owner: 'unload',
        flow_phase: 'retracting',
        updated_ms: 3210,
    }));
    assert.match(await page.locator('#printer-operation-title').textContent(),
                 /AMS 260（回抽当前耗材，Top-AMS 正在退线）/);
    assert.match(await page.locator('#printer-operation-detail').textContent(),
                 /旧通道完整退线/);
    assert.match(await page.locator('#diag-ams').textContent(), /AMS 260/);

    await page.evaluate(() => renderPrinterOperationStatus({
        ams_status_info: {
            raw: 1234, main: 4, sub: 210, known: false,
            label_zh: '未知状态：主码 4，子码 210',
        },
        printer_fault: { active: false },
        printer_status_ready: true,
        channel_confirmed: true,
        hw_switch: 1,
        current_channel: 2,
        motor_owner: 'none',
        flow_phase: 'idle',
        updated_ms: 4000,
    }));
    assert.match(await page.locator('#printer-operation-title').textContent(),
                 /AMS 1234（未知状态：主码 4，子码 210）/);

    await page.evaluate(() => renderPrinterOperationStatus({
        ams_status_info: {
            raw: 262, main: 1, sub: 6, known: true,
            label_zh: '外挂料模式下等待确认喷嘴是否出料',
        },
        printer_fault: {
            active: true,
            interlock: true,
            source: 'hms',
            display_code: '07FF8010',
            label_zh: '外挂料盘或耗材卡住',
            action_zh: '检查料盘是否顺畅、PTFE 管是否弯折及料路是否受阻。',
            severity: 'error',
        },
        printer_status_ready: true,
        channel_confirmed: true,
        hw_switch: 1,
        current_channel: 2,
        motor_owner: 'none',
        flow_phase: 'fault',
        updated_ms: 5000,
    }));
    assert.match(await page.locator('#printer-operation-banner').getAttribute('class'),
                 /danger/);
    assert.match(await page.locator('#printer-operation-title').textContent(),
                 /07FF8010/);
    assert.match(await page.locator('#printer-operation-detail').textContent(),
                 /电机已停止，打印保持暂停/);

    await page.evaluate(() => {
        latestTimeline = {
            run_id: 7,
            state: 'error',
            from_channel: 2,
            to_channel: 4,
            started_ms: 1000,
            finished_ms: 5000,
            stages: [
                { id: 'trigger', state: 'success', started_ms: 1000, finished_ms: 1100, detail: '换料触发已确认' },
                { id: 'unload', state: 'success', started_ms: 1100, finished_ms: 1800, detail: '收到 AMS 260' },
                { id: 'retract', state: 'success', started_ms: 1800, finished_ms: 3000, detail: '传感器稳定无料' },
                { id: 'heat', state: 'success', started_ms: 3000, finished_ms: 4000, detail: '实际温度已确认' },
                { id: 'feed', state: 'error', started_ms: 4000, finished_ms: 5000, detail: '07FF8010 料盘卡住' },
                { id: 'resume', state: 'not_reached', detail: '保持打印暂停' },
            ],
            last_timeout: null,
        };
        renderFilamentTimeline();
        showTab('main-panel');
    });
    assert.match(await page.locator('#timeline-printer-action').textContent(), /AMS 262/);
    assert.match(await page.locator('[data-stage="feed"] .timeline-meta').textContent(),
                 /07FF8010/);

    await page.screenshot({
        path: path.join(outputDir, 'top-ams-status-desktop.png'),
        fullPage: true,
    });
    await page.setViewportSize({ width: 390, height: 844 });
    await page.screenshot({
        path: path.join(outputDir, 'top-ams-status-390px.png'),
        fullPage: true,
    });
    const sizes = await page.evaluate(() => ({
        viewport: document.documentElement.clientWidth,
        content: document.documentElement.scrollWidth,
        bannerHeight: document.querySelector('#printer-operation-banner').offsetHeight,
    }));
    assert.ok(sizes.content <= sizes.viewport,
              `390px layout overflows horizontally: ${JSON.stringify(sizes)}`);
    assert.ok(sizes.bannerHeight > 0, 'operation banner is not visible');
    assert.deepEqual(pageErrors, []);

    console.log('Web UI interaction and 1440/390px layout tests passed');
    } finally {
        if (browser)
            await browser.close();
        await new Promise((resolve) => server.close(resolve));
    }
}

main().catch((error) => {
    console.error(error);
    process.exitCode = 1;
});
