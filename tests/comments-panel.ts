// The Comments sidebar view: annotations grouped by page (or author, status,
// ...), replies and review status in the card, filtered by text and by author.
//
// Run: bun tests/comments-panel.ts [--no-build]

import { mkdirSync, rmSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { ControlCommand, type ControlClient } from "./control.ts";
import { runStandalone, tmpPath } from "./util";
import { sleep } from "./winapi";
import { killAndWait, launchControlled } from "./win-automation";

// page 1: alice's note with bob's reply and carol's Accepted status; page 2:
// bob's highlight and a link, which the panel leaves out
function makePdf(): string {
  const objs: string[] = [];
  objs[1] = "<< /Type /Catalog /Pages 2 0 R >>";
  objs[2] = "<< /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >>";
  objs[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [4 0 R 5 0 R 9 0 R] >>";
  objs[4] = "<< /Type /Annot /Subtype /Text /Rect [20 240 40 260] /T (alice) /Contents (first note) >>";
  objs[5] = "<< /Type /Annot /Subtype /Text /Rect [20 240 40 260] /T (bob) /Contents (agreed) /IRT 4 0 R >>";
  objs[6] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [7 0 R 8 0 R] >>";
  objs[7] =
    "<< /Type /Annot /Subtype /Highlight /Rect [20 200 120 220] /QuadPoints [20 220 120 220 20 200 120 200] " +
    "/T (bob) /Contents (check this) >>";
  objs[8] = "<< /Type /Annot /Subtype /Link /Rect [20 100 120 120] /A << /S /URI /URI (https://example.com) >> >>";
  objs[9] =
    "<< /Type /Annot /Subtype /Text /Rect [20 240 40 260] /T (carol) /Contents (Accepted set by carol) " +
    "/IRT 4 0 R /StateModel /Review /State /Accepted >>";

  let pdf = "%PDF-1.7\n";
  const offsets: number[] = [];
  for (let i = 1; i < objs.length; i++) {
    offsets[i] = pdf.length;
    pdf += `${i} 0 obj\n${objs[i]}\nendobj\n`;
  }
  const xref = pdf.length;
  pdf += `xref\n0 ${objs.length}\n0000000000 65535 f \n`;
  for (let i = 1; i < objs.length; i++) {
    pdf += `${String(offsets[i]).padStart(10, "0")} 00000 n \n`;
  }
  pdf += `trailer\n<< /Size ${objs.length} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`;
  return pdf;
}

async function panel(client: ControlClient, action = "", arg = ""): Promise<string> {
  const res = await client.request(ControlCommand.TestCommentsPanel, [action, arg]);
  return String(res[1] ?? "");
}

function rows(state: string): string[] {
  return state
    .split("\n")
    .filter((l) => l.startsWith("row "))
    .map((l) => l.slice(4));
}

function expectRows(what: string, state: string, want: string[]) {
  const got = rows(state);
  if (got.join("\n") !== want.join("\n")) {
    throw new Error(`comments-panel: ${what}: want\n${want.join("\n")}\ngot\n${got.join("\n")}\n(${state})`);
  }
}

// annotations load in the background and the tree rebuilds after
async function waitForRows(client: ControlClient, what: string, n: number): Promise<string> {
  let state = "";
  for (let i = 0; i < 100; i++) {
    state = await panel(client);
    if (/loaded=1/.test(state) && rows(state).length === n) {
      return state;
    }
    await sleep(50);
  }
  throw new Error(`comments-panel: ${what}: wanted ${n} rows: ${state}`);
}

const allRows = [
  "Page 1 (1)",
  "  Text (alice): first note [Accepted (carol)]",
  "    bob: agreed",
  "Page 2 (1)",
  "  Highlight (bob): check this",
];

export async function testit(): Promise<void> {
  const dir = tmpPath("comments-panel");
  rmSync(dir, { recursive: true, force: true });
  mkdirSync(dir, { recursive: true });
  const pdf = join(dir, "comments.pdf");
  writeFileSync(pdf, makePdf(), "latin1");

  const { proc, client } = await launchControlled(["-view", "single page", "-zoom", "fit page", pdf]);
  try {
    await client.waitForRenderIdle();
    await client.setNotificationsEnabled(false);
    await client.request(ControlCommand.TestInvokeCommand, ["CmdToggleComments"]);

    let state = await waitForRows(client, "opened", allRows.length);
    if (!/shown=1/.test(state) || !/authors=2/.test(state)) {
      throw new Error(`comments-panel: not shown, or not 2 authors: ${state}`);
    }
    expectRows("all", state, allRows);

    // a thread shows when the comment or any reply matches
    expectRows("author alice", await panel(client, "author", "alice"), allRows.slice(0, 3));
    expectRows("author bob", await panel(client, "author", "bob"), allRows);
    expectRows("all authors", await panel(client, "author", ""), allRows);
    expectRows("filter", await panel(client, "filter", "check"), allRows.slice(3));
    expectRows("filter cleared", await panel(client, "filter", ""), allRows);

    state = await panel(client, "choose", "4");
    if (!/selected=Highlight \(bob\): check this/.test(state)) {
      throw new Error(`comments-panel: choosing the highlight didn't select it: ${state}`);
    }
    // replies aren't drawn: choosing one selects its comment
    state = await panel(client, "choose", "2");
    if (!/selected=Text \(alice\): first note/.test(state)) {
      throw new Error(`comments-panel: choosing the reply didn't select its comment: ${state}`);
    }

    expectRows("by author", await panel(client, "group", "author"), [
      "alice (1)",
      "  Text (alice): first note [Accepted (carol)]",
      "    bob: agreed",
      "bob (1)",
      "  Highlight (bob): check this",
    ]);
    expectRows("by status", await panel(client, "group", "status"), [
      "No status (1)",
      "  Highlight (bob): check this",
      "Accepted (1)",
      "  Text (alice): first note [Accepted (carol)]",
      "    bob: agreed",
    ]);
    expectRows("by page", await panel(client, "group", "page"), allRows);

    // a collapsed group stays collapsed across rebuilds
    await panel(client, "toggle", "0");
    expectRows("collapsed", await panel(client, "rebuild"), ["Page 1 (1) collapsed", ...allRows.slice(3)]);
    expectRows("expanded", await panel(client, "toggle", "0"), allRows);

    // a status is a hidden reply: the newest one shows, none is listed
    await panel(client, "status", "4 Completed");
    state = await waitForRows(client, "after status", allRows.length);
    if (!/row {3}Highlight \(bob\): check this \[Completed( \(.+\))?\]/.test(state)) {
      throw new Error(`comments-panel: status not shown: ${state}`);
    }
    await panel(client, "status", "1 Rejected");
    state = await waitForRows(client, "after second status", allRows.length);
    if (!/row {3}Text \(alice\): first note \[Rejected( \(.+\))?\]/.test(state)) {
      throw new Error(`comments-panel: newer status didn't win: ${state}`);
    }

    // a reply typed in the card
    await panel(client, "reply", "4 noted");
    state = await waitForRows(client, "after reply", allRows.length + 1);
    if (!/row {5}.*noted/.test(state)) {
      throw new Error(`comments-panel: reply not added: ${state}`);
    }

    // deleting a comment takes its reply along
    await panel(client, "delete", "1");
    state = await waitForRows(client, "after delete", 3);
    if (rows(state)[0] !== "Page 2 (1)" || /alice|carol/.test(state)) {
      throw new Error(`comments-panel: after delete: ${state}`);
    }
    // bob and whoever wrote the reply; alice went with her comment
    if (!/authors=2/.test(state)) {
      throw new Error(`comments-panel: the author list kept a deleted author: ${state}`);
    }
  } finally {
    client.close();
    await killAndWait(proc);
  }
}

if (import.meta.main) {
  await runStandalone(testit);
}
