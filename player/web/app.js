'use strict'

const startScreen = document.getElementById('start')
const gameScreen = document.getElementById('game')
const pickButton = document.getElementById('pick')
const recentButton = document.getElementById('recent')
const recentName = document.getElementById('recent-name')
const romInput = document.getElementById('rom-input')
const canvas = document.getElementById('canvas')
const stage = document.getElementById('stage')
const logEl = document.getElementById('log')
const fullscreenButton = document.getElementById('fullscreen')
const exportSaveButton = document.getElementById('export-save')
const importSaveButton = document.getElementById('import-save')
const changeRomButton = document.getElementById('change-rom')
const saveInput = document.getElementById('save-input')

canvas.addEventListener('contextmenu', (event) => event.preventDefault())

const DB_NAME = 'ds-player'
const DB_VERSION = 1
const RECENT_STORE = 'recent'
const RECENT_KEY = 'rom'

let engine = null
let romName = ''

function log(text) {
  logEl.textContent += text + '\n'
  logEl.scrollTop = logEl.scrollHeight
}

function openDb() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, DB_VERSION)
    request.onupgradeneeded = () =>
      request.result.createObjectStore(RECENT_STORE)
    request.onsuccess = () => resolve(request.result)
    request.onerror = () => reject(request.error)
  })
}

async function readRecent() {
  const db = await openDb()
  return new Promise((resolve, reject) => {
    const request = db
      .transaction(RECENT_STORE, 'readonly')
      .objectStore(RECENT_STORE)
      .get(RECENT_KEY)
    request.onsuccess = () => resolve(request.result || null)
    request.onerror = () => reject(request.error)
  })
}

async function storeRecent(name, bytes) {
  const db = await openDb()
  return new Promise((resolve, reject) => {
    const tx = db.transaction(RECENT_STORE, 'readwrite')
    tx.objectStore(RECENT_STORE).put({ name, bytes }, RECENT_KEY)
    tx.oncomplete = () => resolve()
    tx.onerror = () => reject(tx.error)
  })
}

function syncSaves(populate) {
  return new Promise((resolve) => {
    engine.FS.syncfs(populate, (error) => {
      if (error) console.warn('[save] sync failed:', error)
      resolve()
    })
  })
}

function savePath() {
  return '/saves/' + romName.replace(/\.[^.]*$/, '') + '.sav'
}

function safeFileName(name) {
  return name.replace(/[\\/]/g, '_') || 'game.nds'
}

async function startGame(name, bytes, options = {}) {
  romName = safeFileName(name)
  startScreen.hidden = true
  gameScreen.hidden = false
  log('Loading the emulator…')

  try {
    engine = await DSPlayerModule({
      canvas,
      print: log,
      printErr: (text) => log(text)
    })
  } catch (error) {
    log('Could not load the emulator: ' + error)
    return
  }

  const FS = engine.FS
  FS.mkdir('/saves')
  FS.mount(FS.filesystems.IDBFS, {}, '/saves')
  await syncSaves(true)
  FS.mkdir('/roms')
  FS.writeFile('/roms/' + romName, bytes)

  log('Starting ' + romName + '…')
  canvas.focus()
  try {
    engine.callMain(['/roms/' + romName])
  } catch (error) {
    if (error !== 'unwind') log('The emulator exited: ' + error)
  }

  if (!options.fromRecent) {
    storeRecent(romName, bytes).catch((error) =>
      console.warn('[recent] could not remember the ROM:', error)
    )
  }
}

pickButton.addEventListener('click', () => romInput.click())

romInput.addEventListener('change', async () => {
  const file = romInput.files && romInput.files[0]
  romInput.value = ''
  if (!file) return
  const bytes = new Uint8Array(await file.arrayBuffer())
  startGame(file.name, bytes)
})

recentButton.addEventListener('click', async () => {
  const recent = await readRecent()
  if (!recent) {
    recentButton.hidden = true
    return
  }
  startGame(recent.name, new Uint8Array(recent.bytes), { fromRecent: true })
})

readRecent()
  .then((recent) => {
    if (!recent) return
    recentButton.hidden = false
    recentName.hidden = false
    recentName.textContent = 'Recent: ' + recent.name
  })
  .catch(() => {})

fullscreenButton.addEventListener('click', () => {
  if (document.fullscreenElement) document.exitFullscreen()
  else stage.requestFullscreen().catch(() => {})
  canvas.focus()
})

exportSaveButton.addEventListener('click', () => {
  if (!engine) return
  engine._ds_player_flush_save()
  let bytes
  try {
    bytes = engine.FS.readFile(savePath())
  } catch (error) {
    log('There is no save for this game yet.')
    return
  }
  const link = document.createElement('a')
  link.href = URL.createObjectURL(new Blob([bytes]))
  link.download = savePath().slice('/saves/'.length)
  link.click()
  URL.revokeObjectURL(link.href)
  canvas.focus()
})

importSaveButton.addEventListener('click', () => saveInput.click())

saveInput.addEventListener('change', async () => {
  const file = saveInput.files && saveInput.files[0]
  saveInput.value = ''
  if (!file || !engine) return
  engine._ds_player_flush_save()
  engine.FS.writeFile(savePath(), new Uint8Array(await file.arrayBuffer()))
  await syncSaves(false)
  engine._ds_player_reload()
  log('Imported ' + file.name + ' and restarted the game.')
  canvas.focus()
})

changeRomButton.addEventListener('click', () => {
  if (engine) engine._ds_player_flush_save()
  location.reload()
})

// Write any pending save before the tab goes away.
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'hidden' && engine)
    engine._ds_player_flush_save()
})
window.addEventListener('pagehide', () => {
  if (engine) engine._ds_player_flush_save()
})
