// REDLINE build-story deck. Run: node build_deck.js
const pptxgen = require("pptxgenjs");
const React = require("react");
const ReactDOMServer = require("react-dom/server");
const sharp = require("sharp");
const fa = require("react-icons/fa");
const path = require("path");

const REPO = "/home/davidj/Claude Data/redline";
const SHOT = (n) => path.join(REPO, "build", n);

// Palette: near-black ground, blood red, brass gold, bone text.
const C = {
  bg: "0B0A0F", panel: "17151C", panel2: "221F28", red: "D7261E", redDark: "8C1711",
  gold: "F2C14E", text: "EDE7E3", muted: "A39E99", dim: "6E6A70", green: "7BC96F",
};
const HEAD = "Arial";
const BODY = "Calibri";
const MONO = "Courier New";

async function icon(Comp, colorHex, px = 256) {
  const svg = ReactDOMServer.renderToStaticMarkup(React.createElement(Comp, { color: "#" + colorHex, size: px }));
  const buf = await sharp(Buffer.from(svg)).png().toBuffer();
  return "image/png;base64," + buf.toString("base64");
}

(async () => {
  const pres = new pptxgen();
  pres.layout = "LAYOUT_16x9"; // 10 x 5.625
  pres.author = "David Janice";
  pres.title = "REDLINE: building a game with AI";

  const W = 10, H = 5.625;
  const icons = {
    spec: await icon(fa.FaClipboardList, C.gold),
    skeleton: await icon(fa.FaCubes, C.gold),
    delegate: await icon(fa.FaUsers, C.gold),
    integrate: await icon(fa.FaPuzzlePiece, C.gold),
    verify: await icon(fa.FaRobot, C.gold),
    commit: await icon(fa.FaCodeBranch, C.gold),
    bug: await icon(fa.FaBug, C.text),
    music: await icon(fa.FaMusic, C.gold),
    chip: await icon(fa.FaMicrochip, C.gold),
    wave: await icon(fa.FaWaveSquare, C.gold),
    flask: await icon(fa.FaFlask, C.gold),
    shield: await icon(fa.FaShieldAlt, C.gold),
    gamepad: await icon(fa.FaGamepad, C.gold),
    hand: await icon(fa.FaHandPaper, C.gold),
    eye: await icon(fa.FaEye, C.gold),
    check: await icon(fa.FaCheck, C.green),
    lightbulb: await icon(fa.FaLightbulb, C.gold),
    scissors: await icon(fa.FaCut, C.gold),
    file: await icon(fa.FaFileAlt, C.gold),
    redo: await icon(fa.FaRedo, C.gold),
    terminal: await icon(fa.FaTerminal, C.gold),
    comments: await icon(fa.FaComments, C.gold),
  };

  const bg = (slide) => { slide.background = { color: C.bg }; };
  const title = (slide, t, opts = {}) =>
    slide.addText(t, { x: 0.5, y: 0.35, w: 9, h: 0.7, fontFace: HEAD, fontSize: 30, bold: true, color: C.text, isTextBox: true, margin: 0, ...opts });
  const kicker = (slide, t) =>
    slide.addText(t, { x: 0.5, y: 0.12, w: 9, h: 0.25, fontFace: BODY, fontSize: 11, color: C.red, bold: true, charSpacing: 4, isTextBox: true, margin: 0 });
  const frame = (slide, img, x, y, w, h) => {
    slide.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: x - 0.05, y: y - 0.05, w: w + 0.1, h: h + 0.1, fill: { color: C.panel2 }, line: { color: C.redDark, width: 1 }, rectRadius: 0.08, shadow: { type: "outer", color: C.red, blur: 12, offset: 0, opacity: 0.35 } });
    slide.addImage({ path: img, x, y, w, h, rounding: false });
  };
  const notes = (slide, t) => slide.addNotes(t);
  const footer = (slide, n) =>
    slide.addText(`REDLINE  ·  built with Claude Code  ·  ${n}`, { x: 0.5, y: H - 0.35, w: 9, h: 0.25, fontFace: BODY, fontSize: 9, color: C.dim, isTextBox: true, margin: 0, align: "right" });

  // 1. Title ------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    s.addImage({ path: SHOT("shot_fps.png"), x: 0, y: 0, w: W, h: H, transparency: 60 });
    s.addShape(pres.shapes.RECTANGLE, { x: 0, y: 0, w: W, h: H, fill: { color: C.bg, transparency: 30 } });
    s.addText("REDLINE", { x: 0.7, y: 1.35, w: 8.6, h: 1.2, fontFace: HEAD, fontSize: 66, bold: true, color: C.red, isTextBox: true, margin: 0 });
    s.addText("How an AI built a Doom-flavoured Tetris in one afternoon", { x: 0.7, y: 2.55, w: 8.6, h: 0.6, fontFace: HEAD, fontSize: 24, color: C.text, isTextBox: true, margin: 0 });
    s.addText("A Claude Code case study for the engineering team  ·  September 2026", { x: 0.7, y: 3.2, w: 8.6, h: 0.4, fontFace: BODY, fontSize: 14, color: C.gold, isTextBox: true, margin: 0 });
    s.addText("Native Vulkan 1.3  ·  C++20  ·  22 commits  ·  ~12,000 lines  ·  one working day", { x: 0.7, y: 4.6, w: 8.6, h: 0.35, fontFace: BODY, fontSize: 12, color: C.muted, isTextBox: true, margin: 0 });
    notes(s, "Frame the talk: this is not a demo of the game, it is a demo of a way of working. Everything you will see was produced from plain-English requests in a single session on 14 September 2026.");
  }

  // 2. The brief --------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "STEP 0");
    title(s, "It started as one paragraph");
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.5, y: 1.2, w: 4.6, h: 3.5, fill: { color: C.panel }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
    s.addText([
      { text: "“A Tetris-style game that uses Vulkan… every once in a while the game switches to a first-person Doom-style mode when certain blocks refuse to explode. Red blocks go all the way across, the player flies in with a gun, and the blocks turn into enemies. When they are destroyed they take the regular blocks around them with them. We can use assets from the Doom games.”", options: { italic: true, color: C.text, fontSize: 13, breakLine: true } },
      { text: " ", options: { fontSize: 6, breakLine: true } },
      { text: "That was the whole specification. No architecture, no stack beyond “Vulkan” and “RTX Remix”, no art. The AI had to decide the rest and say so when the request and reality disagreed.", options: { color: C.muted, fontSize: 12 } },
    ], { x: 0.75, y: 1.35, w: 4.1, h: 3.2, fontFace: BODY, valign: "top", isTextBox: true, margin: 0 });
    frame(s, SHOT("shot_alert.png"), 5.45, 1.2, 4.05, 2.28);
    s.addText("The red row completes; the board is about to tip over.", { x: 5.45, y: 3.55, w: 4.05, h: 0.3, fontFace: BODY, fontSize: 10, color: C.muted, italic: true, isTextBox: true, margin: 0 });
    s.addText([
      { text: "First decision: ", options: { bold: true, color: C.gold } },
      { text: "RTX Remix is a Windows D3D9 runtime, not a Linux Vulkan SDK. The AI said so up front, built a native Vulkan renderer with a Remix-shaped material model, and documented the Windows path instead of pretending.", options: { color: C.text } },
    ], { x: 5.45, y: 3.95, w: 4.05, h: 1.1, fontFace: BODY, fontSize: 11, valign: "top", isTextBox: true, margin: 0 });
    footer(s, 2);
    notes(s, "Read the quote. Point out how little was specified. The RTX Remix correction is the first example of the AI pushing back rather than guessing.");
  }

  // 3. By the numbers -----------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "THE OUTPUT");
    title(s, "One session, by the numbers");
    const stats = [
      ["7.5 h", "wall clock, first\ncommit to last"],
      ["22", "commits, each a\nplayable increment"],
      ["12.4k", "lines of C++20,\nGLSL and tests"],
      ["304", "automated checks\nin four test suites"],
      ["2", "subagents delegated\nself-contained modules"],
    ];
    stats.forEach(([n, l], i) => {
      const x = 0.5 + i * 1.84;
      s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y: 1.35, w: 1.7, h: 2.1, fill: { color: C.panel }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
      s.addText(n, { x, y: 1.5, w: 1.7, h: 0.95, fontFace: HEAD, fontSize: 40, bold: true, color: C.gold, align: "center", isTextBox: true, margin: 0 });
      s.addText(l, { x: x + 0.1, y: 2.5, w: 1.5, h: 0.8, fontFace: BODY, fontSize: 11, color: C.muted, align: "center", valign: "top", isTextBox: true, margin: 0 });
    });
    s.addText([
      { text: "What shipped: ", options: { bold: true, color: C.gold } },
      { text: "a Vulkan renderer, a Doom WAD reader, a rules engine, a first-person combat sim with seven monster classes and four weapons, an OPL3 synthesizer for Doom's music, streamed Ogg soundtracks, gamepad support, player profiles, trophies, high scores, and display options. All from conversation.", options: { color: C.text } },
    ], { x: 0.5, y: 3.75, w: 9, h: 1.2, fontFace: BODY, fontSize: 13, valign: "top", isTextBox: true, margin: 0 });
    footer(s, 3);
    notes(s, "Numbers come from git log and wc. Stress the test count: the AI wrote its own regression net as it went, not afterwards.");
  }

  // 4. Workflow -------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "THE METHOD");
    title(s, "The loop that repeated all afternoon");
    const steps = [
      ["Specify", "Turn the request into concrete rules and interfaces", icons.spec],
      ["Skeleton + tests", "Pure logic first, unit tests before pixels", icons.skeleton],
      ["Delegate", "Self-contained modules to subagents with a written contract", icons.delegate],
      ["Integrate", "Wire the pieces, build with -Wall -Wextra clean", icons.integrate],
      ["Verify", "Scripted scenarios, a bot player, screenshots, logs", icons.verify],
      ["Commit + iterate", "Small commit, docs updated, next request", icons.commit],
    ];
    steps.forEach(([h, d, ic], i) => {
      const col = i % 3, row = Math.floor(i / 3);
      const x = 0.5 + col * 3.05, y = 1.3 + row * 1.85;
      s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w: 2.85, h: 1.6, fill: { color: C.panel }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
      s.addShape(pres.shapes.OVAL, { x: x + 0.2, y: y + 0.2, w: 0.55, h: 0.55, fill: { color: C.redDark } });
      s.addImage({ data: ic, x: x + 0.32, y: y + 0.32, w: 0.31, h: 0.31 });
      s.addText(`${i + 1}`, { x: x + 2.3, y: y + 0.15, w: 0.4, h: 0.4, fontFace: HEAD, fontSize: 18, bold: true, color: C.dim, align: "right", isTextBox: true, margin: 0 });
      s.addText(h, { x: x + 0.9, y: y + 0.2, w: 1.5, h: 0.5, fontFace: HEAD, fontSize: 14, bold: true, color: C.text, valign: "middle", isTextBox: true, margin: 0 });
      s.addText(d, { x: x + 0.2, y: y + 0.85, w: 2.5, h: 0.65, fontFace: BODY, fontSize: 11, color: C.muted, valign: "top", isTextBox: true, margin: 0 });
    });
    footer(s, 4);
    notes(s, "Every feature request went through these six steps. The important habit is step 5: the AI does not declare something done until a run proves it.");
  }

  // 5. Foundations ----------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "STEP 1  ·  12:30 – 12:56");
    title(s, "Foundations before pixels");
    s.addText([
      { text: "Rules engine first, renderer second", options: { bold: true, color: C.gold, fontSize: 15, breakLine: true } },
      { text: "The block game lives in a dependency-free module with a deterministic seed. It was written and unit-tested before a single Vulkan call existed, so every later rule change had a place to be proven.", options: { color: C.text, fontSize: 12, breakLine: true } },
      { text: " ", options: { fontSize: 8, breakLine: true } },
      { text: "What the first hour produced", options: { bold: true, color: C.gold, fontSize: 15, breakLine: true } },
      { text: "CMake build with shaders compiled and embedded at build time", options: { bullet: true, color: C.text, fontSize: 12, breakLine: true, paraSpaceAfter: 4 } },
      { text: "Vulkan 1.3 context: dynamic rendering, two frames in flight, screenshot read-back", options: { bullet: true, color: C.text, fontSize: 12, breakLine: true, paraSpaceAfter: 4 } },
      { text: "Instanced cubes and billboards from one texture atlas", options: { bullet: true, color: C.text, fontSize: 12, breakLine: true, paraSpaceAfter: 4 } },
      { text: "Procedural fallback art and sounds so it runs without a WAD", options: { bullet: true, color: C.text, fontSize: 12, breakLine: true, paraSpaceAfter: 4 } },
      { text: "Nine rule tests that grew to 119 checks by the end of the day", options: { bullet: true, color: C.text, fontSize: 12 } },
    ], { x: 0.5, y: 1.25, w: 5.2, h: 3.7, fontFace: BODY, valign: "top", isTextBox: true, margin: 0 });
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 6.0, y: 1.25, w: 3.5, h: 3.7, fill: { color: C.panel }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
    s.addText("tests/tetris_test.cpp", { x: 6.2, y: 1.35, w: 3.1, h: 0.3, fontFace: MONO, fontSize: 10, color: C.gold, isTextBox: true, margin: 0 });
    s.addText([
      "testClassicLineClear",
      "testRedCellSurvivesClear",
      "testRedCellsStayInPiece",
      "testRedLineTrigger",
      "testRedRowDoesNotClear",
      "testRedRegions",
      "testScoring / testCascadeChain",
      "testCorruption",
      "testEvilSpawn",
      "testPanicAndPurge",
      "testPrizes",
      "testDeterminism",
    ].map((t, i, a) => ({ text: t, options: { breakLine: i < a.length - 1, color: C.text } })),
      { x: 6.2, y: 1.7, w: 3.1, h: 3.1, fontFace: MONO, fontSize: 10.5, valign: "top", isTextBox: true, margin: 0, paraSpaceAfter: 3 });
    footer(s, 5);
    notes(s, "The rules engine is the contract. Point at the test list: each name is a rule the user asked for, expressed as code that fails if the rule regresses.");
  }

  // 6. Delegation -------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "STEP 2  ·  DELEGATION");
    title(s, "Hand off a module, keep the contract");
    frame(s, SHOT("shot_countdown.png"), 0.5, 1.25, 4.4, 2.475);
    s.addText("Real Doom sprites, textures, font and sounds, decoded by the delegated WAD reader.", { x: 0.5, y: 3.8, w: 4.4, h: 0.4, fontFace: BODY, fontSize: 10, color: C.muted, italic: true, isTextBox: true, margin: 0 });
    s.addText([
      { text: "The WAD reader was written by a subagent", options: { bold: true, color: C.gold, fontSize: 15, breakLine: true } },
      { text: "While the main session built the renderer, a second agent got a written spec: the exact public API, the binary formats, robustness rules, and a synthetic-WAD test to write first.", options: { color: C.text, fontSize: 12, breakLine: true } },
      { text: " ", options: { fontSize: 8, breakLine: true } },
      { text: "It came back with a report, not just code:", options: { bold: true, color: C.gold, fontSize: 13, breakLine: true } },
      { text: "1,456 checks passing, clean under ASan and UBSan", options: { bullet: true, color: C.text, fontSize: 12, breakLine: true, paraSpaceAfter: 3 } },
      { text: "Validated against the real doom.wad and doom2.wad: 764 sprites, 287 textures, 122 sounds, all decoded", options: { bullet: true, color: C.text, fontSize: 12, breakLine: true, paraSpaceAfter: 3 } },
      { text: "Ten listed deviations from the spec, each explained", options: { bullet: true, color: C.text, fontSize: 12, breakLine: true, paraSpaceAfter: 3 } },
      { text: "Later the same pattern produced the OPL3 synthesizer.", options: { color: C.muted, fontSize: 12 } },
    ], { x: 5.3, y: 1.25, w: 4.2, h: 3.7, fontFace: BODY, valign: "top", isTextBox: true, margin: 0 });
    footer(s, 6);
    notes(s, "Delegation works when the interface is fixed in writing before the work starts. The parent integrated the module without reading its internals.");
  }

  // 7. First loop ---------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "STEP 3  ·  12:56");
    title(s, "The first playable loop");
    const modes = ["Blocks", "Alert", "Fly-in", "Countdown", "Fight", "Fly-out", "Level up"];
    modes.forEach((m, i) => {
      const x = 0.5 + i * 1.3;
      s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y: 1.25, w: 1.15, h: 0.45, fill: { color: i === 4 ? C.redDark : C.panel }, line: { color: C.panel2, width: 1 }, rectRadius: 0.08 });
      s.addText(m, { x, y: 1.25, w: 1.15, h: 0.45, fontFace: HEAD, fontSize: 11, bold: true, color: C.text, align: "center", valign: "middle", isTextBox: true, margin: 0 });
      if (i < modes.length - 1) s.addText("›", { x: x + 1.15, y: 1.25, w: 0.15, h: 0.45, fontFace: HEAD, fontSize: 14, color: C.gold, align: "center", valign: "middle", isTextBox: true, margin: 0 });
    });
    frame(s, SHOT("shot_flyin.png"), 0.5, 1.95, 4.4, 2.475);
    frame(s, SHOT("shot_levelcard.png"), 5.1, 1.95, 4.4, 2.475);
    s.addText("The board hinges over onto the floor; the player lands inside it.", { x: 0.5, y: 4.5, w: 4.4, h: 0.3, fontFace: BODY, fontSize: 10, color: C.muted, italic: true, isTextBox: true, margin: 0 });
    s.addText("Surviving raises the level; the collapse plays behind the card.", { x: 5.1, y: 4.5, w: 4.4, h: 0.3, fontFace: BODY, fontSize: 10, color: C.muted, italic: true, isTextBox: true, margin: 0 });
    s.addText("The first version put the player in front of a standing wall. Forty minutes later a one-line request (“rotate the play space”) turned it into the board tipping over, which needed per-instance rotation in the shader, a new coordinate mapping, and collision against the block grid.", { x: 0.5, y: 4.85, w: 9, h: 0.5, fontFace: BODY, fontSize: 10.5, color: C.text, isTextBox: true, margin: 0 });
    footer(s, 7);
    notes(s, "The whole state machine existed 25 minutes after the first commit. The tip-over redesign shows how a small request can ripple through renderer, physics and camera, and still land the same hour.");
  }

  // 8. Verification ---------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "STEP 5  ·  VERIFICATION");
    title(s, "Proof the machine can run by itself");
    s.addText([
      { text: "Built-in hooks made every feature checkable from a shell", options: { bold: true, color: C.gold, fontSize: 14, breakLine: true } },
      { text: "--scenario redline | fps | corrupt | prize  ·  hand-made boards", options: { bullet: true, color: C.text, fontSize: 11.5, breakLine: true, paraSpaceAfter: 3 } },
      { text: "--bot  ·  an auto-aiming player that finishes fights", options: { bullet: true, color: C.text, fontSize: 11.5, breakLine: true, paraSpaceAfter: 3 } },
      { text: "--frames N --screenshot x.png  ·  deterministic 60 Hz stepping", options: { bullet: true, color: C.text, fontSize: 11.5, breakLine: true, paraSpaceAfter: 3 } },
      { text: "--keys BFG@30x150  ·  scripted key presses and chords", options: { bullet: true, color: C.text, fontSize: 11.5, breakLine: true, paraSpaceAfter: 3 } },
      { text: "REDLINE_MUSIC_DUMP  ·  capture the audio mix to a WAV", options: { bullet: true, color: C.text, fontSize: 11.5, breakLine: true, paraSpaceAfter: 3 } },
      { text: "XDG_DATA_HOME  ·  isolate test save data from the real profile", options: { bullet: true, color: C.text, fontSize: 11.5, breakLine: true, paraSpaceAfter: 6 } },
      { text: "Every claim in the session's reports traced back to one of these runs: a log line, a screenshot the AI looked at, or a rendered WAV.", options: { color: C.muted, fontSize: 11.5 } },
    ], { x: 0.5, y: 1.25, w: 4.6, h: 3.7, fontFace: BODY, valign: "top", isTextBox: true, margin: 0 });
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 5.4, y: 1.25, w: 4.1, h: 3.7, fill: { color: "060608" }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
    s.addText("$ redline --scenario redline --bot --frames 3000", { x: 5.6, y: 1.4, w: 3.8, h: 0.3, fontFace: MONO, fontSize: 9.5, color: C.gold, isTextBox: true, margin: 0 });
    s.addText([
      "[app] mode Blocks -> Alert (frame 0, score 32)",
      "[app] mode Alert -> FlyIn (frame 85)",
      "[app] mode FlyIn -> Countdown (frame 218)",
      "[fps] level 1 roster: DEMON(4), IMP(3),",
      "      DEMON(6), DEMON(5), IMP(3), ZOMBIE(1)",
      "[app] mode Countdown -> Fps (frame 399)",
      "[fps] pickup rocket launcher (health 86)",
      "[fps] rocket blast destroyed 1 blocks",
      "[app] trophy unlocked: FIRST BLOOD",
      "[app] mode Fps -> FlyOut (frame 1183)",
      "[app] mode FlyOut -> Blocks (frame 1268)",
      "[app] trophy unlocked: RED LINE",
    ].map((t, i, a) => ({ text: t, options: { breakLine: i < a.length - 1, color: i % 3 === 0 ? C.text : C.muted } })),
      { x: 5.6, y: 1.75, w: 3.8, h: 3.1, fontFace: MONO, fontSize: 9.5, valign: "top", isTextBox: true, margin: 0, paraSpaceAfter: 2 });
    footer(s, 8);
    notes(s, "This is the slide to dwell on. The AI cannot play the game, so it built ways to make the game prove itself: the bot completed the loop dozens of times during the day.");
  }

  // 9. Iteration timeline ----------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "STEP 6  ·  ITERATING ON FEEDBACK");
    title(s, "Twenty-two commits, one conversation");
    const events = [
      ["12:56", "First playable loop"],
      ["13:37", "Board tips over; monster tiers; menus"],
      ["13:51", "Loot drops, four weapons"],
      ["14:57", "Countdown, cover erosion, absorb-and-grow"],
      ["15:29", "Corruption, combo scoring"],
      ["16:08", "Level caps, region splitting"],
      ["17:24", "Panic mode, hidden BFG"],
      ["18:30", "OPL3 music, high scores"],
      ["19:15", "Gamepad, profiles, trophies"],
      ["20:31", "Display options"],
    ];
    const x0 = 0.7, x1 = 9.3, y = 2.55;
    s.addShape(pres.shapes.LINE, { x: x0, y, w: x1 - x0, h: 0, line: { color: C.redDark, width: 3 } });
    events.forEach(([t, l], i) => {
      const x = x0 + (i / (events.length - 1)) * (x1 - x0);
      s.addShape(pres.shapes.OVAL, { x: x - 0.09, y: y - 0.09, w: 0.18, h: 0.18, fill: { color: C.gold }, line: { color: C.bg, width: 1 } });
      const up = i % 2 === 0;
      s.addText(t, { x: x - 0.5, y: up ? y - 0.55 : y + 0.2, w: 1.0, h: 0.25, fontFace: HEAD, fontSize: 10, bold: true, color: C.gold, align: "center", isTextBox: true, margin: 0 });
      s.addText(l, { x: x - 0.62, y: up ? y - 1.5 : y + 0.45, w: 1.24, h: 0.95, fontFace: BODY, fontSize: 9.5, color: C.text, align: "center", valign: up ? "bottom" : "top", isTextBox: true, margin: 0 });
    });
    s.addText([
      { text: "Each request became a commit with a test where a rule changed, a docs update, and a verification run. ", options: { color: C.text } },
      { text: "Corrections were treated the same way as features: ", options: { color: C.text } },
      { text: "“red blocks break off pieces, that's not right” ", options: { italic: true, color: C.gold } },
      { text: "became a rewritten test, then an engine change, in one commit.", options: { color: C.text } },
    ], { x: 0.5, y: 4.15, w: 9, h: 0.8, fontFace: BODY, fontSize: 11.5, valign: "top", isTextBox: true, margin: 0 });
    footer(s, 9);
    notes(s, "Walk the timeline quickly. The gaps between commits are the conversation: the user played, said what felt wrong, the AI changed it.");
  }

  // 10. Test-first rule change --------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "HOW A RULE CHANGES");
    title(s, "Feedback becomes a test first");
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.5, y: 1.25, w: 4.7, h: 3.7, fill: { color: "060608" }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
    s.addText("// evil spawn: a hole needs a full red surround", { x: 0.7, y: 1.4, w: 4.3, h: 0.25, fontFace: MONO, fontSize: 9, color: C.gold, isTextBox: true, margin: 0 });
    s.addText([
      "fill(g, 19, CellKind::Red, 5);   // bottom row, hole at 5",
      "g.setCell(5, 18, Cell{CellKind::Red, 0});  // red above",
      "auto holes = g.evilSpawnCandidates();",
      "CHECK(holes.size() == 1 && holes[0] == {5, 19});",
      "",
      "// open sky above never qualifies",
      "Game h(4, r);  fill(h, 19, CellKind::Red, 5);",
      "CHECK(h.evilSpawnCandidates().empty());",
      "",
      "// mid-board: all four neighbours must be red",
      "m.setCell(4, 11, Cell{CellKind::Normal, 0});",
      "CHECK(m.evilSpawnCandidates().empty());",
      "m.setCell(4, 11, Cell{CellKind::Red, 0});",
      "CHECK(m.evilSpawnCandidates().size() == 1);",
    ].map((t, i, a) => ({ text: t, options: { breakLine: i < a.length - 1, color: t.startsWith("//") ? C.muted : C.text } })),
      { x: 0.7, y: 1.7, w: 4.3, h: 3.15, fontFace: MONO, fontSize: 8.5, valign: "top", isTextBox: true, margin: 0, paraSpaceAfter: 1 });
    frame(s, SHOT("shot_evilspawn.png"), 5.5, 1.25, 4.0, 2.25);
    s.addText([
      { text: "The user asked: ", options: { bold: true, color: C.gold } },
      { text: "“it has to have a full surround before spawning, right? Unless it's the bottom row.” ", options: { italic: true, color: C.text } },
      { text: "The first version had been looser. The fix started with the cases above, and the engine was changed until they passed. The old, wrong behaviour can never come back silently.", options: { color: C.text } },
    ], { x: 5.5, y: 3.65, w: 4.0, h: 1.35, fontFace: BODY, fontSize: 11, valign: "top", isTextBox: true, margin: 0 });
    footer(s, 10);
    notes(s, "This is the discipline to sell: the correction is encoded as a test with the exact edge cases the user described, then the code follows.");
  }

  // 11. Music ---------------------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "DEEP DIVE  ·  18:30");
    title(s, "Music from a chip written to spec");
    const rows = [
      [icons.file, "MUS + GENMIDI", "Doom's music is stored as compact MIDI-like scores with an Adlib instrument bank. Both parsers were written and tested against the real WAD."],
      [icons.chip, "OPL3 emulator", "No soundfont on the machine, so a subagent implemented the Yamaha YMF262 from its documented behaviour: log-sin tables, envelopes, feedback, 18 voices. 48 checks including a 440 Hz pitch measurement; 1.7% of one core."],
      [icons.wave, "Three soundtracks", "The rerelease ships Ogg recordings in a 625 MB WAD. The game indexes it by directory only and streams the Modern (Hulshult) or SC-55 sets; Classic falls back to the emulator."],
      [icons.music, "Verified by ear and by numbers", "Tracks were rendered to WAV for the user to hear, and the mix was captured in-game to prove the streaming path."],
    ];
    rows.forEach(([ic, h, d], i) => {
      const y = 1.25 + i * 0.93;
      s.addShape(pres.shapes.OVAL, { x: 0.5, y: y + 0.08, w: 0.6, h: 0.6, fill: { color: C.redDark } });
      s.addImage({ data: ic, x: 0.64, y: y + 0.22, w: 0.32, h: 0.32 });
      s.addText(h, { x: 1.3, y, w: 8.2, h: 0.3, fontFace: HEAD, fontSize: 13, bold: true, color: C.gold, isTextBox: true, margin: 0 });
      s.addText(d, { x: 1.3, y: y + 0.3, w: 8.2, h: 0.6, fontFace: BODY, fontSize: 11, color: C.text, valign: "top", isTextBox: true, margin: 0 });
    });
    footer(s, 11);
    notes(s, "The point is not that the AI knows about OPL chips. It is that when the easy route (install a soundfont) needed sudo, it took the harder route that would work immediately, and told the user how to get the easy route too.");
  }

  // 12. Bugs the process caught ---------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "WHAT THE PROCESS CAUGHT");
    title(s, "Bugs found by the AI's own checks");
    const bugs = [
      ["Crash on exit", "Audio streams were destroyed after SDL_Quit. Every earlier run used --mute, so it hid until a run with sound checked its exit code.", "Explicit shutdown order"],
      ["Crash on exit, again", "The music stream had the same lifetime bug. Found because the previous fix made exit codes something the AI now checked every time.", "Music::shutdown() before SDL_Quit"],
      ["Segfault with --fullscreen", "Display settings applied before the Vulkan context existed. Caught by the very run meant to prove the new option.", "Null guard; context sizes itself"],
      ["A test that lied", "A helper swapped the queued piece, not the falling one, so a combo test passed for the wrong reason. Spotted when a stronger assertion failed.", "Helper fixed, assertions tightened"],
    ];
    bugs.forEach(([h, d, f], i) => {
      const col = i % 2, row = Math.floor(i / 2);
      const x = 0.5 + col * 4.6, y = 1.25 + row * 1.85;
      s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w: 4.4, h: 1.65, fill: { color: C.panel }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
      s.addShape(pres.shapes.OVAL, { x: x + 0.2, y: y + 0.2, w: 0.5, h: 0.5, fill: { color: C.red } });
      s.addImage({ data: icons.bug, x: x + 0.31, y: y + 0.31, w: 0.28, h: 0.28 });
      s.addText(h, { x: x + 0.85, y: y + 0.2, w: 3.4, h: 0.5, fontFace: HEAD, fontSize: 13, bold: true, color: C.text, valign: "middle", isTextBox: true, margin: 0 });
      s.addText(d, { x: x + 0.2, y: y + 0.78, w: 4.0, h: 0.6, fontFace: BODY, fontSize: 10, color: C.muted, valign: "top", isTextBox: true, margin: 0 });
      s.addText("Fix: " + f, { x: x + 0.2, y: y + 1.35, w: 4.0, h: 0.25, fontFace: BODY, fontSize: 10, color: C.gold, isTextBox: true, margin: 0 });
    });
    footer(s, 12);
    notes(s, "None of these were reported by the user. They were found because the AI's habit is to run the thing and read the exit code, not to assume.");
  }

  // 13. Pushback and honesty ----------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "JUDGEMENT");
    title(s, "Where the AI said no, or said “not verified”");
    const items = [
      [icons.hand, "RTX Remix", "Asked for “Vulkan + RTX Remix SDK”. Explained that Remix is a Windows D3D9 runtime, built Vulkan natively, and shaped the renderer so a Remix backend is a drop-in later."],
      [icons.eye, "Controller support", "Implemented against SDL3's gamepad API, but no pad was attached. The report says so plainly instead of claiming it was tested."],
      [icons.terminal, "Things it cannot do", "Installing a soundfont needs sudo. It gave the one-line command and built the no-dependency route so music worked immediately anyway."],
      [icons.comments, "Design calls", "Interpretations that changed gameplay (tipping the board, what “surround” means) were stated as assumptions and revised when the user clarified."],
    ];
    items.forEach(([ic, h, d], i) => {
      const y = 1.25 + i * 0.93;
      s.addShape(pres.shapes.OVAL, { x: 0.5, y: y + 0.08, w: 0.6, h: 0.6, fill: { color: C.redDark } });
      s.addImage({ data: ic, x: 0.64, y: y + 0.22, w: 0.32, h: 0.32 });
      s.addText(h, { x: 1.3, y, w: 4.2, h: 0.3, fontFace: HEAD, fontSize: 13, bold: true, color: C.gold, isTextBox: true, margin: 0 });
      s.addText(d, { x: 1.3, y: y + 0.3, w: 4.4, h: 0.62, fontFace: BODY, fontSize: 10.5, color: C.text, valign: "top", isTextBox: true, margin: 0 });
    });
    frame(s, SHOT("shot_options.png"), 6.1, 1.35, 3.4, 1.9125);
    s.addText("Options screen: music sets, controller settings, display modes, all persisted per player or per machine.", { x: 6.1, y: 3.35, w: 3.4, h: 0.55, fontFace: BODY, fontSize: 9.5, color: C.muted, italic: true, valign: "top", isTextBox: true, margin: 0 });
    footer(s, 13);
    notes(s, "Trust comes from the AI distinguishing what it did, what it verified, and what it could not. Engineers should expect and demand that distinction.");
  }

  // 14. Architecture ---------------------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "WHAT GOT BUILT");
    title(s, "Architecture and where the lines went");
    s.addChart(pres.charts.BAR, [{
      name: "Lines", labels: ["game", "audio", "render", "core", "wad", "tests", "tools", "shaders"],
      values: [4665, 2023, 1318, 1077, 957, 1878, 301, 196],
    }], {
      x: 0.5, y: 1.2, w: 4.9, h: 3.8, barDir: "bar", chartColors: [C.red],
      showTitle: true, title: "Lines per module", titleColor: C.text, titleFontSize: 12, titleFontFace: HEAD,
      showValue: true, dataLabelPosition: "outEnd", dataLabelColor: C.text, dataLabelFontSize: 9,
      catAxisLabelColor: C.text, catAxisLabelFontSize: 10, valAxisLabelColor: C.dim, valAxisLabelFontSize: 8,
      valGridLine: { color: "2A2730", size: 0.5 }, catGridLine: { style: "none" }, showLegend: false,
      plotArea: { fill: { color: C.bg } }, chartArea: { fill: { color: C.bg } },
    });
    const mods = [
      ["core", "Rules engine, image and PNG helpers. No dependencies."],
      ["wad", "Doom WAD reader: patches, sprites, textures, fonts, DMX sounds."],
      ["render", "Vulkan 1.3 context, atlas packer, instanced renderer."],
      ["audio", "SDL3 mixer, MUS sequencer, GENMIDI, OPL3 emulator, Ogg streaming."],
      ["game", "Assets, FPS simulation, app state machine, HUD, profiles, trophies."],
    ];
    mods.forEach(([h, d], i) => {
      const y = 1.3 + i * 0.72;
      s.addText(h, { x: 5.8, y, w: 1.0, h: 0.3, fontFace: MONO, fontSize: 12, bold: true, color: C.gold, isTextBox: true, margin: 0 });
      s.addText(d, { x: 6.8, y, w: 2.7, h: 0.62, fontFace: BODY, fontSize: 10.5, color: C.text, valign: "top", isTextBox: true, margin: 0 });
    });
    footer(s, 14);
    notes(s, "Layering kept the AI's changes local: gameplay rules never touched the renderer, and the audio stack was added without changing the game loop.");
  }

  // 15. Lessons -------------------------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    kicker(s, "TAKEAWAYS");
    title(s, "What to copy into your own AI-assisted work");
    const lessons = [
      [icons.spec, "Write the contract down", "Interfaces, formats and edge cases in prose before code. It is what makes delegation and review possible."],
      [icons.flask, "Tests are the memory", "Every rule the user cares about becomes a check. Corrections become tests first."],
      [icons.robot ?? icons.verify, "Give the program ways to prove itself", "Scenarios, bots, screenshot flags, dumps. If the AI can't observe it, it can't verify it."],
      [icons.scissors, "Small commits, real messages", "Each increment playable; the history reads as the story of the design."],
      [icons.shield, "Demand the honesty line", "Done, verified, and unverified are three different words. Insist on all three."],
      [icons.redo, "Keep the human on design", "The AI proposed mechanics; the user decided what felt right and said so in one sentence."],
    ];
    lessons.forEach(([ic, h, d], i) => {
      const col = i % 3, row = Math.floor(i / 3);
      const x = 0.5 + col * 3.05, y = 1.25 + row * 1.85;
      s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w: 2.85, h: 1.65, fill: { color: C.panel }, line: { color: C.panel2, width: 1 }, rectRadius: 0.1 });
      s.addShape(pres.shapes.OVAL, { x: x + 0.2, y: y + 0.18, w: 0.5, h: 0.5, fill: { color: C.redDark } });
      s.addImage({ data: ic, x: x + 0.31, y: y + 0.29, w: 0.28, h: 0.28 });
      s.addText(h, { x: x + 0.85, y: y + 0.15, w: 1.9, h: 0.55, fontFace: HEAD, fontSize: 12, bold: true, color: C.text, valign: "middle", isTextBox: true, margin: 0 });
      s.addText(d, { x: x + 0.2, y: y + 0.78, w: 2.5, h: 0.8, fontFace: BODY, fontSize: 10, color: C.muted, valign: "top", isTextBox: true, margin: 0 });
    });
    footer(s, 15);
    notes(s, "Close on the practices, not the game. These six habits are what made the afternoon productive and what will transfer to real product work.");
  }

  // 16. Try it -----------------------------------------------------------------------------------------
  {
    const s = pres.addSlide();
    bg(s);
    s.addImage({ path: SHOT("shot_cyber.png"), x: 0, y: 0, w: W, h: H, transparency: 60 });
    s.addShape(pres.shapes.RECTANGLE, { x: 0, y: 0, w: W, h: H, fill: { color: C.bg, transparency: 40 } });
    s.addText("Try it", { x: 0.7, y: 0.7, w: 8.6, h: 0.8, fontFace: HEAD, fontSize: 40, bold: true, color: C.red, isTextBox: true, margin: 0 });
    s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x: 0.7, y: 1.6, w: 8.6, h: 0.55, fill: { color: "060608" }, line: { color: C.panel2, width: 1 }, rectRadius: 0.08 });
    s.addText('cd "/home/davidj/Claude Data/redline" && cmake -S . -B build -G Ninja && ninja -C build && ./build/redline', { x: 0.9, y: 1.6, w: 8.2, h: 0.55, fontFace: MONO, fontSize: 11, color: C.gold, valign: "middle", isTextBox: true, margin: 0 });
    s.addText([
      { text: "Needs: ", options: { bold: true, color: C.gold } },
      { text: "Arch/CachyOS packages cmake, ninja, gcc, vulkan-headers, shaderc, sdl3, glm, and a Doom IWAD (found automatically from Steam). Full docs in README.md and docs/design.md.", options: { color: C.text, breakLine: true } },
      { text: " ", options: { fontSize: 6, breakLine: true } },
      { text: "Next on the list: ", options: { bold: true, color: C.gold } },
      { text: "ray-query shadows on the Vulkan path, and the RTX Remix backend on Windows, both already scoped in docs/remix.md.", options: { color: C.text } },
    ], { x: 0.7, y: 2.4, w: 8.6, h: 1.3, fontFace: BODY, fontSize: 13, valign: "top", isTextBox: true, margin: 0 });
    s.addText("Questions? Ask the AI to walk you through any commit: the history is the documentation.", { x: 0.7, y: 4.6, w: 8.6, h: 0.4, fontFace: BODY, fontSize: 12, color: C.muted, italic: true, isTextBox: true, margin: 0 });
    notes(s, "End with the invitation: clone it, run it, read the git log. Every design decision is a commit message with a test next to it.");
  }

  const out = path.join(REPO, "docs", "REDLINE-build-story.pptx");
  await pres.writeFile({ fileName: out });
  console.log("wrote", out);
})();
