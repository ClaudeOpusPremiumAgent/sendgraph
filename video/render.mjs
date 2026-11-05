// Renders film.html frame by frame. Runs in the Playwright image; see render.sh.
// Usage: node render.mjs preview 3 22 41   (stills at those seconds)
//        node render.mjs film
import { chromium } from "playwright";
import { writeFileSync } from "node:fs";

const FPS = 30;
const [mode, ...rest] = process.argv.slice(2);
const browser = await chromium.launch();
const report = await browser.newPage({ viewport: { width: 1120, height: 800 } });
await report.goto("http://localhost:8000/video/out/report-plain.html");
await report.screenshot({ path: "/out/report.png" });
await report.close();
const page = await browser.newPage({ viewport: { width: 1920, height: 1080 } });
page.on("pageerror", (e) => console.log("pageerror", e.message));
await page.goto("http://localhost:8000/video/film.html");
await page.waitForFunction(() => window.DURATION);
await page.evaluate(() => document.fonts.ready);
const info = await page.evaluate(() => ({ duration: window.DURATION, timeline: window.TIMELINE }));
console.log("duration", info.duration);
writeFileSync("/out/timeline.json", JSON.stringify(info.timeline));
const overflow = () =>
  page.evaluate(() =>
    [...document.querySelectorAll(".scene .a")].filter((n) => n.offsetParent).flatMap((n) => {
      const r = n.getBoundingClientRect();
      return r.height && (r.bottom > 1080 || r.right > 1920) ? [`${n.textContent.trim().slice(0, 40)} bottom=${Math.round(r.bottom)}`] : [];
    }));
if (mode === "preview") {
  for (const t of rest.map(Number)) {
    await page.evaluate((x) => window.seek(x), t);
    console.log(t, await overflow());
    await page.screenshot({ path: `/out/still-${t}.jpg`, type: "jpeg", quality: 70 });
  }
} else {
  const total = Math.round(info.duration * FPS);
  for (let frame = 0; frame < total; frame++) {
    await page.evaluate((x) => window.seek(x), frame / FPS);
    await page.screenshot({ path: `/out/frames/${String(frame).padStart(5, "0")}.jpg`, type: "jpeg", quality: 90 });
    if (frame % 600 === 0) console.log(`frame ${frame}/${total}`);
  }
}
await browser.close();
