// Browser tests for the dashboard (M3.1). A real daemon serves the built web app,
// a real Chromium loads it, and the tests use the page the way a person would:
// by label, role and keyboard. Nothing is mocked.
//
// Environment: VEYLQORA_BIN (the daemon binary), VEYLQORA_FIXTURES (an indexed
// fixture root, not yet indexed here), WEB_DIST (the built web app).

import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, describe, it } from "node:test";

import chromiumPackage from "@sparticuz/chromium";
import { chromium } from "playwright-core";

const BIN = process.env.VEYLQORA_BIN;
const FIXTURES = process.env.VEYLQORA_FIXTURES;
const DIST = process.env.WEB_DIST;
if (!BIN || !FIXTURES || !DIST) throw new Error("set VEYLQORA_BIN, VEYLQORA_FIXTURES and WEB_DIST");

let work;
let daemon;
let base;
let browser;
let page;
const consoleErrors = [];

function startDaemon() {
  return new Promise((resolve, reject) => {
    const proc = spawn(BIN, ["serve", "--db", join(work, "dash.db"), "--port", "0", "--web", DIST]);
    let output = "";
    proc.stdout.on("data", (chunk) => {
      output += chunk;
      const match = output.match(/listening on (http:\/\/127\.0\.0\.1:\d+)/);
      if (match) resolve({ proc, url: match[1] });
    });
    proc.on("exit", (code) => reject(new Error(`daemon exited with ${code}: ${output}`)));
  });
}

async function api(method, path, body) {
  const response = await fetch(base + path, {
    method,
    headers: { "content-type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return response.json();
}

before(async () => {
  work = mkdtempSync(join(tmpdir(), "veylqora-browser-"));
  ({ proc: daemon, url: base } = await startDaemon());
  await api("POST", "/api/v1/index", { root: FIXTURES });
  await api("POST", "/api/v1/memories", {
    root: FIXTURES,
    type: "decision",
    content: "browser test memory about token checks",
    author: "browser-test",
    scope: "shared",
    evidence: [{ rel_path: "auth/tokens.py", symbol_fqn: "auth.tokens.verify_token" }],
  });
  browser = await chromium.launch({
    executablePath: await chromiumPackage.executablePath(),
    args: chromiumPackage.args,
    headless: true,
  });
  page = await browser.newPage();
  page.on("console", (message) => {
    if (message.type() === "error") consoleErrors.push(message.text());
  });
  page.on("pageerror", (error) => consoleErrors.push(String(error)));
  await page.goto(base + "/");
  await page.getByLabel("Project root").fill(FIXTURES);
  await page.getByRole("button", { name: "Open" }).click();
});

after(async () => {
  await browser?.close();
  daemon?.kill();
  rmSync(work, { recursive: true, force: true });
});

describe("dashboard in a real browser", () => {
  it("opens a project and shows the overview from the status endpoint", async () => {
    await page.getByText("Overview").first().waitFor();
    const status = await api("GET", `/api/v1/status?root=${encodeURIComponent(FIXTURES)}`);
    await page.getByText(String(status.symbols), { exact: true }).first().waitFor();
  });

  it("inspects a query across the three retrieval modes with scores", async () => {
    await page.getByRole("tab", { name: "Retrieval" }).click();
    await page.getByLabel("Query").fill("token verification");
    await page.getByRole("button", { name: "Inspect" }).click();
    for (const mode of ["hybrid", "lexical", "vector"]) {
      const region = page.getByRole("region", { name: `${mode} ranking` });
      await region.getByText("auth.tokens.verify_token").waitFor();
      const text = await region.textContent();
      assert.match(text, /score \d+\.\d{4}/, `${mode} shows a score`);
    }
  });

  it("moves between tabs with the arrow keys", async () => {
    // Arrow keys move from the selected tab, so select Overview first.
    const overview = page.getByRole("tab", { name: "Overview" });
    await overview.click();
    await overview.focus();
    await page.keyboard.press("ArrowRight");
    assert.equal(await page.getByRole("tab", { name: "Search" }).getAttribute("aria-selected"), "true");
  });

  it("lists memories and shows a timeline for the selected one", async () => {
    await page.getByRole("tab", { name: "Memories" }).click();
    await page.getByRole("button", { name: "browser test memory about token checks" }).click();
    const timeline = page.getByRole("list", { name: "timeline" });
    await timeline.waitFor();
    assert.ok((await timeline.getByRole("listitem").count()) >= 1, "timeline has entries");
  });

  it("draws the code graph of a symbol and keeps the text list beside it", async () => {
    // check_password calls hash_password in the fixture, so the graph has an edge.
    await page.getByRole("tab", { name: "Graph" }).click();
    await page.getByLabel("Symbol name").fill("check_password");
    await page.getByRole("button", { name: "Show" }).click();
    await page.getByRole("img", { name: /Graph of check_password/ }).waitFor();
    const dependencies = page.getByRole("region", { name: "dependencies" });
    await dependencies.getByText("auth.passwords.hash_password").waitFor();
    const canvas = await page.locator(".graph-canvas canvas").count();
    assert.ok(canvas >= 1, "the graph is drawn on a canvas");
  });

  it("logs no console errors while using the dashboard", () => {
    assert.deepEqual(consoleErrors, []);
  });
});
