// With DocumentColorsFollowTheme = smart the page background must differ from
// the canvas around it, or the page edge disappears (no frame is drawn).
//
// Run: bun tests/smart-page-boundary.ts [--no-build]   (or via tests/run-almost-all.ts)

import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { withControlledSumatra } from "./control.ts";
import { EXE, runStandalone, tmpPath } from "./util.ts";
import { captureWindowPixels } from "./winapi.ts";
import { findCanvas, waitForFrame } from "./win-automation.ts";

// first three built-in themes used to be tinted, the rest weren't
const THEMES = ["Light", "Dark", "Dark from 3.5", "Charcoal"];
const MIN_DELTA = 10;

function blankPdf(): Buffer {
  const objs = [
    "<</Type/Catalog/Pages 2 0 R>>",
    "<</Type/Pages/Kids[3 0 R]/Count 1>>",
    "<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]>>",
  ];
  let out = "%PDF-1.4\n";
  const offsets: number[] = [];
  objs.forEach((o, i) => {
    offsets.push(out.length);
    out += `${i + 1} 0 obj\n${o}\nendobj\n`;
  });
  const xrefAt = out.length;
  out += `xref\n0 ${objs.length + 1}\n0000000000 65535 f \n`;
  for (const off of offsets) {
    out += `${String(off).padStart(10, "0")} 00000 n \n`;
  }
  out += `trailer\n<</Size ${objs.length + 1}/Root 1 0 R>>\nstartxref\n${xrefAt}\n%%EOF\n`;
  return Buffer.from(out, "latin1");
}

function pixelAt(cap: { w: number; data: Uint8Array }, x: number, y: number): number[] {
  const i = (y * cap.w + x) * 4;
  return [cap.data[i + 2]!, cap.data[i + 1]!, cap.data[i]!];
}

const hex = (c: number[]) => "#" + c.map((v) => v.toString(16).padStart(2, "0")).join("");

async function checkTheme(dir: string, pdfPath: string, theme: string): Promise<void> {
  const appData = join(dir, `appdata-${theme.replace(/\W+/g, "-")}`);
  mkdirSync(appData, { recursive: true });
  writeFileSync(
    join(appData, "SumatraPDF-settings.txt"),
    [
      "UiLanguage = en",
      "CheckForUpdates = false",
      "RestoreSession = false",
      `Theme = ${theme}`,
      "DocumentColorsFollowTheme = smart",
      "",
    ].join("\n"),
  );

  await withControlledSumatra(
    EXE,
    async (client, proc) => {
      const frame = await waitForFrame(proc.pid!);
      await client.waitForRenderIdle(30000);
      await client.setNotificationsEnabled(false);
      const canvas = findCanvas(frame);
      if (!canvas) {
        throw new Error(`smart-page-boundary: no canvas (${theme})`);
      }
      const cap = captureWindowPixels(canvas);
      if (!cap) {
        throw new Error(`smart-page-boundary: capture failed (${theme})`);
      }
      // fit page in a landscape window: the left edge is canvas, the center is page
      const y = Math.floor(cap.h / 2);
      const around = pixelAt(cap, 3, y);
      const page = pixelAt(cap, Math.floor(cap.w / 2), y);
      const delta = Math.max(...around.map((v, i) => Math.abs(v - page[i]!)));
      if (delta < MIN_DELTA) {
        throw new Error(
          `smart-page-boundary: page ${hex(page)} blends with canvas ${hex(around)} in '${theme}' (delta=${delta})`,
        );
      }
      console.log(`  ${theme}: page ${hex(page)} canvas ${hex(around)} ✓`);
    },
    ["-appdata", appData, "-view", "single page", "-zoom", "fit page", pdfPath],
  );
}

export async function testit(): Promise<void> {
  const dir = tmpPath("smart-page-boundary");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const pdfPath = join(dir, "blank.pdf");
  writeFileSync(pdfPath, blankPdf());
  for (const theme of THEMES) {
    await checkTheme(dir, pdfPath, theme);
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
