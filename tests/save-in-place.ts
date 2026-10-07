// CmdSave (File > Save, Ctrl+S) writes unsaved annotations into the open PDF
// without a Save As dialog.
//
// Run: bun tests/save-in-place.ts [--no-build]

import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlClient, ControlCommand } from "./control.ts";
import { assemblePdf, cmdId, runStandalone, tmpPath } from "./util.ts";
import { packCoords, sendMessage, sleep, WM_COMMAND } from "./winapi.ts";
import { killAndWait, launchControlled, sendCommand } from "./win-automation.ts";

function makePdf(): string {
  return assemblePdf([
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Annots [4 0 R] >>",
    "<< /Type /Annot /Subtype /Square /P 3 0 R /Rect [72 420 192 540] /C [1 0 0] /BS << /W 2 >> >>",
  ]);
}

type Markup = { annotations: number; square: { x: number; y: number } | null };

async function markup(client: ControlClient): Promise<Markup> {
  const deadline = Date.now() + 10_000;
  for (;;) {
    const res = await client.request(ControlCommand.TestMarkupAnnots, []);
    const raw = String(res[1] ?? "");
    const count = /annotations=(\d+)/.exec(raw);
    if (res[0] === 0 && count) {
      const m = /type=Square[^\n]*screen=(-?\d+),(-?\d+),(-?\d+),(-?\d+)/.exec(raw);
      const square = m ? { x: +m[1]! + Math.floor(+m[3]! / 2), y: +m[2]! + Math.floor(+m[4]! / 2) } : null;
      return { annotations: +count[1]!, square };
    }
    if (Date.now() > deadline) {
      throw new Error(`save-in-place: could not read annotations\n${raw}`);
    }
    await sleep(100);
  }
}

async function waitForLog(logPath: string, re: RegExp, what: string): Promise<void> {
  const deadline = Date.now() + 15_000;
  for (;;) {
    const log = existsSync(logPath) ? readFileSync(logPath, "utf8") : "";
    if (re.test(log)) {
      return;
    }
    if (Date.now() > deadline) {
      throw new Error(`save-in-place: ${what}; log:\n${log.slice(-3000)}`);
    }
    await sleep(100);
  }
}

export async function testit(): Promise<void> {
  const dir = tmpPath("save-in-place");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const pdf = join(dir, "doc.pdf");
  const logPath = join(dir, "log.txt");
  const original = Buffer.from(makePdf(), "latin1");
  writeFileSync(pdf, original);

  const { proc, client, frame } = await launchControlled(["-log-to-file", logPath, pdf]);
  try {
    await client.waitForRenderIdle();
    await client.setNotificationsEnabled(false);
    sendCommand(frame, cmdId("CmdToggleEditPDF"));
    await sleep(300);

    const before = await markup(client);
    if (before.annotations !== 1 || !before.square) {
      throw new Error(`save-in-place: expected one square, got ${before.annotations}`);
    }
    sendMessage(frame, WM_COMMAND, cmdId("CmdDeleteAnnotation"), packCoords(before.square.x, before.square.y));

    // a Save As dialog would block here and the save would never be logged
    sendCommand(frame, cmdId("CmdSave"));
    await waitForLog(logPath, /Saved annotations to [^\n]*doc\.pdf/, "CmdSave did not save into the open file");

    if (readFileSync(pdf).equals(original)) {
      throw new Error("save-in-place: file unchanged after CmdSave");
    }
    await waitForLog(logPath, /ReloadDocument: [^\n]*reloaded in/, "the saved file was not reloaded");
    const reloaded = await markup(client);
    if (reloaded.annotations !== 0) {
      throw new Error(`save-in-place: reloaded file has ${reloaded.annotations} annotation(s), want 0`);
    }
    console.log("save-in-place: OK");
  } finally {
    client.close();
    await killAndWait(proc);
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
