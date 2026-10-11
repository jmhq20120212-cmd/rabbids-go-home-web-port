// Register before the game runtime starts. The worker controls the second load.
if ((location.protocol === 'https:' || ['localhost', '127.0.0.1'].includes(location.hostname)) &&
    !crossOriginIsolated && 'serviceWorker' in navigator) {
  let reloading = false;
  navigator.serviceWorker.addEventListener('controllerchange', () => {
    if (!reloading) { reloading = true; location.reload(); }
  });
  navigator.serviceWorker.register('./coi-sw.js', { scope: './', updateViaCache: 'none' })
    .then((registration) => {
      // When a previous version controls the page, a reload also repairs it.
      if (navigator.serviceWorker.controller && !crossOriginIsolated) registration.update();
    })
    .catch((err) => { console.error('Cannot enable cross-origin isolation:', err); });
}
