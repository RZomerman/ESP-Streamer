const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const html = fs.readFileSync(path.join(__dirname, "..", "data", "www", "radio.html"), "utf8");
const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)];
const elements = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    textContent: "", hidden: false, checked: false, disabled: false, value: "",
    dataset: {}, setAttribute() {}, addEventListener() {}
  });
  return elements.get(id);
}
const buttons = [0, 1, 2, 3].map(station => ({
  ...element("preset-" + station), dataset: {station: String(station)}
}));
buttons.push(element("custom-play"));
const context = vm.createContext({
  document: {
    getElementById: element,
    querySelectorAll: selector => selector === "button" ? buttons : buttons.slice(0, 4)
  },
  fetch: () => new Promise(() => {}),
  setTimeout() {},
  AbortSignal,
  console
});
vm.runInContext(scripts.at(-1)[1], context);

const radio = {
  selected: 0, source: "Radio", station: "NPO Radio 2", radio_playing: true,
  spdif_gpio: 14, error: "", now_playing: {}
};
const airplay = {
  ...radio, protocol_version: 2, source: "AirPlay", radio_playing: false,
  spdif_gpio: 21, coax_gpio: 47, radio_changes_blocked: true,
  airplay: {playing: true},
  now_playing: {title: "<song>", artist: "Artist", album: "Album"}
};

async function main() {
  context.render(radio);
  assert.equal(element("override-option").hidden, true);
  assert(buttons.every(button => !button.disabled));
  assert.match(element("airplay-policy").textContent, /queued/);

  context.render(airplay);
  assert.equal(element("override-option").hidden, false);
  assert(buttons.every(button => button.disabled));
  assert.match(element("output-detail").textContent, /GPIO21 \/ coax GPIO47/);
  assert.equal(element("track-title").textContent, "<song>");
  context.render({...airplay, airplay: {playing: false}});
  assert(buttons.every(button => button.disabled));
  assert.match(element("detail").textContent, /Paused/);

  element("override-airplay").checked = true;
  context.render(airplay);
  assert(buttons.every(button => !button.disabled));
  let sent;
  context.fetch = async (_url, options) => {
    sent = JSON.parse(options.body);
    return {ok: true, json: async () => ({...radio, protocol_version: 2})};
  };
  await context.select(0);
  assert.deepEqual(sent, {station: 0, override: true});
  assert.equal(element("override-airplay").checked, false);
  assert(buttons.every(button => !button.disabled));

  context.render({...radio, protocol_version: 2});
  await context.select(4, "https://example.org/live");
  assert.deepEqual(sent, {station: 4, url: "https://example.org/live"});

  context.render(radio);
  element("override-airplay").checked = true;
  await context.select(0);
  assert.deepEqual(sent, {station: 0});
  console.log("Radio webpage tests passed: V1 compatibility, V2 playing/paused policy, explicit override and custom URL payload");
}
main().catch(error => { console.error(error); process.exitCode = 1; });
