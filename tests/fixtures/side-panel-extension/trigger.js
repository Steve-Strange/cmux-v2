async function exampleTab() {
  const tabs = await chrome.tabs.query({ currentWindow: true });
  return tabs.find((candidate) =>
    candidate.url?.startsWith("https://example.com/"),
  );
}

document.querySelector("#open").addEventListener("click", async () => {
  const tab = await exampleTab();
  if (!tab?.id) {
    document.querySelector("#result").textContent = "example.com tab missing";
    return;
  }

  await chrome.sidePanel.setOptions({
    tabId: tab.id,
    path: "panel.html",
    enabled: true,
  });
  await chrome.sidePanel.open({ tabId: tab.id });
  document.querySelector("#result").textContent = `opened tab ${tab.id}`;
});

document.querySelector("#close").addEventListener("click", async () => {
  const tab = await exampleTab();
  if (!tab?.id) {
    document.querySelector("#result").textContent = "example.com tab missing";
    return;
  }

  await chrome.sidePanel.close({ tabId: tab.id });
  document.querySelector("#result").textContent = `closed tab ${tab.id}`;
});
