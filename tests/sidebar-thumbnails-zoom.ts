// Ctrl + wheel over the sidebar's thumbnails resizes them, within limits,
// and leaves the document alone. A touchpad's small deltas add up to a step.
//
// Run: bun tests/sidebar-thumbnails-zoom.ts [--no-build]

import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand } from "./control.ts";
import { cmdId, makePdf, runStandalone, tmpPath } from "./util.ts";
import { clientToScreen, getScrollPos, packCoords, sendMessage, SB_VERT, sleep } from "./winapi.ts";
import { findCanvas, killAndWait, launchControlled, sendCommand } from "./win-automation.ts";

const WM_MOUSEWHEEL = 0x020a;
const MK_CONTROL = 0x0008;
const WHEEL_DELTA = 120;
const TOUCHPAD_DELTA = 30;

type Thumbs = { hwnd: number; shown: boolean; page1Dx: number; raw: string };

async function thumbs(client: ControlClient): Promise<Thumbs> {
  const res = await client.request(ControlCommand.TestSidebarThumbnails, []);
  const raw = String(res[1] ?? "");
  const m = /hwnd=(\d+) thumbnails=(\d).* rects=1:-?\d+,-?\d+,(\d+),/.exec(raw);
  if (res[0] !== 0 || !m) {
    throw new Error(`sidebar-thumbnails-zoom: TestSidebarThumbnails: ${raw}`);
  }
  return { hwnd: +m[1]!, shown: m[2] === "1", page1Dx: +m[3]!, raw };
}

async function waitFor(what: string, f: () => Promise<boolean>) {
  const deadline = Date.now() + 5000;
  while (!(await f())) {
    if (Date.now() > deadline) {
      throw new Error(`sidebar-thumbnails-zoom: ${what}`);
    }
    await sleep(50);
  }
}

function ctrlWheel(hwnd: number, delta: number) {
  const pt = clientToScreen(hwnd, 40, 60);
  const wp = BigInt(((delta & 0xffff) << 16) | MK_CONTROL);
  sendMessage(hwnd, WM_MOUSEWHEEL, wp, packCoords(pt.x, pt.y));
}

export async function testit(): Promise<void> {
  const dir = tmpPath("sidebar-thumbnails-zoom");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const pdf = join(dir, "doc.pdf");
  writeFileSync(pdf, makePdf(12, 601, 3, 0), "latin1");

  const { proc, client, frame } = await launchControlled(["-zoom", "400", pdf]);
  try {
    await client.waitForRenderIdle();
    await client.setNotificationsEnabled(false);
    sendCommand(frame, cmdId("CmdToggleThumbnails"));
    await waitFor("Thumbnails didn't show", async () => (await thumbs(client)).shown);
    const canvas = findCanvas(frame);
    const y0 = getScrollPos(canvas, SB_VERT);

    const t0 = await thumbs(client);
    ctrlWheel(t0.hwnd, WHEEL_DELTA);
    let t = await thumbs(client);
    if (t.page1Dx <= t0.page1Dx) {
      throw new Error(`sidebar-thumbnails-zoom: Ctrl + wheel up didn't enlarge: ${t0.page1Dx} -> ${t.page1Dx}`);
    }

    // 4 touchpad deltas make one notch: back to the start size
    for (let i = 0; i < 4; i++) {
      ctrlWheel(t0.hwnd, -TOUCHPAD_DELTA);
    }
    t = await thumbs(client);
    if (t.page1Dx !== t0.page1Dx) {
      throw new Error(`sidebar-thumbnails-zoom: touchpad zoom out: ${t0.page1Dx} -> ${t.page1Dx}`);
    }

    // zooming out stops at the minimum, half the default size
    for (let i = 0; i < 20; i++) {
      ctrlWheel(t0.hwnd, -WHEEL_DELTA);
    }
    t = await thumbs(client);
    if (Math.abs(t.page1Dx - t0.page1Dx / 2) > 1) {
      throw new Error(`sidebar-thumbnails-zoom: min size ${t.page1Dx}, default ${t0.page1Dx}`);
    }

    await sleep(200);
    const y = getScrollPos(canvas, SB_VERT);
    if (y !== y0) {
      throw new Error(`sidebar-thumbnails-zoom: Ctrl + wheel scrolled the document: ${y0} -> ${y}`);
    }
    console.log("sidebar-thumbnails-zoom: OK");
  } finally {
    client.close();
    await killAndWait(proc);
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
