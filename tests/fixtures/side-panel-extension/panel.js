chrome.tabs.query({ active: true, currentWindow: true }).then(([tab]) => {
  document.querySelector("#state").textContent = JSON.stringify(
    {
      tabId: tab?.id ?? null,
      windowId: tab?.windowId ?? null,
      sidePanelApi: typeof chrome.sidePanel?.open === "function",
    },
    null,
    2,
  );
});
