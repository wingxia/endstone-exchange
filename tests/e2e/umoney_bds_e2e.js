'use strict'

const crypto = require('node:crypto')
const { spawnSync } = require('node:child_process')
const bedrock = require('bedrock-protocol')

const host = process.env.E2E_HOST || '127.0.0.1'
const port = Number(process.env.E2E_PORT || 19141)
const username = process.env.E2E_USERNAME || 'UmoneyE2E'
const version = process.env.E2E_VERSION || '1.26.30'
const commandDelay = Number(process.env.E2E_COMMAND_DELAY_MS || 2500)
const commandVersion = process.env.E2E_COMMAND_VERSION ?? ''
const packetMode = process.env.E2E_PACKET_MODE || 'command_request'
const dispatchMode = process.env.E2E_DISPATCH_MODE || 'console_driver'
const screenSession = process.env.E2E_SCREEN_SESSION || ''
const formResponses = (process.env.E2E_FORM_RESPONSES || '')
  .split('\n')
  .map(value => value.trim())
  .filter(Boolean)
const commands = (process.env.E2E_COMMANDS || [
  '/exchange balance',
  '/exchange status',
  '/exchange deposit 100',
  '/exchange balance',
  '/exchange withdraw 40',
  '/exchange balance',
  `/exchange addbalance ${username} 10`
].join('\n')).split('\n').map(value => value.trim()).filter(Boolean)

const stableUuid = value => {
  const bytes = crypto.createHash('sha256').update(`endstone-exchange-e2e:${value}`).digest().subarray(0, 16)
  bytes[6] = (bytes[6] & 0x0f) | 0x40
  bytes[8] = (bytes[8] & 0x3f) | 0x80
  const hex = bytes.toString('hex')
  return [
    hex.slice(0, 8),
    hex.slice(8, 12),
    hex.slice(12, 16),
    hex.slice(16, 20),
    hex.slice(20)
  ].join('-')
}

const clientIdentity = process.env.E2E_CLIENT_ID || stableUuid(username)

const json = value => JSON.stringify(value, (_key, item) => {
  if (typeof item === 'bigint') return `${item}n`
  if (Buffer.isBuffer(item)) return { buffer: item.toString('hex') }
  return item
})

const client = bedrock.createClient({
  host,
  port,
  username,
  offline: true,
  version,
  skinData: {
    SelfSignedId: clientIdentity,
    DeviceId: clientIdentity,
    PlayFabId: clientIdentity.replaceAll('-', '').slice(0, 16)
  }
})

let finished = false
let failed = false
let commandOutputs = 0
let textPackets = 0
let formPackets = 0
let ranAllCommands = false
let playerEntityId = 0n

const sleep = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds))

const finish = exitCode => {
  if (finished) return
  finished = true
  try {
    client.disconnect('UMoney E2E complete')
  } catch (_error) {
    // The peer may already be closed after an earlier protocol failure.
  }
  setTimeout(() => process.exit(exitCode), 250)
}

const sendCommand = command => {
  console.log(`SEND ${command}`)
  if (dispatchMode === 'console_driver') {
    if (!screenSession) {
      throw new Error('E2E_SCREEN_SESSION is required for console_driver mode')
    }
    const match = command.match(/^\/?exchange(?:\s+(.*))?$/)
    if (!match || !match[1]) {
      throw new Error(`console_driver only accepts /exchange commands: ${command}`)
    }
    const line = `exchangee2e ${username} ${match[1]}`
    const result = spawnSync(
      'screen',
      ['-S', screenSession, '-p', '0', '-X', 'stuff', `\u0015${line}\r`],
      { encoding: 'utf8' }
    )
    if (result.error || result.status !== 0) {
      throw result.error || new Error(
        `screen dispatch failed (${result.status}): ${result.stderr || result.stdout}`
      )
    }
    return
  }
  if (packetMode === 'text') {
    client.write('text', {
      needs_translation: false,
      category: 'authored',
      type: 'chat',
      source_name: username,
      message: command,
      xuid: '',
      platform_chat_id: '',
      has_filtered_message: false
    })
    return
  }
  if (packetMode === 'legacy_command_request') {
    client.write('command_request', {
      command,
      origin: {
        type: 'player',
        uuid: process.env.E2E_COMMAND_UUID || 'fd8f8f8f-8f8f-8f8f-8f8f-8f8f8f8f8f8f',
        request_id: ''
      },
      interval: false
    })
    return
  }
  client.write('command_request', {
    command,
    origin: {
      type: 'player',
      uuid: process.env.E2E_COMMAND_UUID || client.profile.uuid || crypto.randomUUID(),
      request_id: '',
      player_entity_id: process.env.E2E_COMMAND_ENTITY_ID === 'runtime'
        ? (client.entityId || 0n)
        : playerEntityId
    },
    internal: false,
    version: commandVersion
  })
}

client.on('command_output', packet => {
  commandOutputs += 1
  console.log(`COMMAND_OUTPUT ${json(packet)}`)
})

client.on('start_game', packet => {
  playerEntityId = packet.entity_id
})

client.on('text', packet => {
  textPackets += 1
  console.log(`TEXT ${json(packet)}`)
})

client.on('modal_form_request', packet => {
  formPackets += 1
  console.log(`FORM ${json(packet)}`)
  const response = formResponses[formPackets - 1]
  if (response !== undefined) {
    setTimeout(() => {
      client.write('modal_form_response', {
        form_id: packet.form_id,
        has_response_data: true,
        data: response,
        has_cancel_reason: false
      })
      console.log(`FORM_RESPONSE ${response}`)
    }, 100)
  }
})

client.on('error', error => {
  failed = true
  console.error(`ERROR ${error.stack || error}`)
})

client.on('kick', packet => {
  failed = true
  console.error(`KICK ${json(packet)}`)
})

client.on('spawn', async () => {
  console.log(`SPAWN ${json({ clientIdentity, entityId: client.entityId, username })}`)
  await sleep(1500)
  for (const command of commands) {
    try {
      sendCommand(command)
    } catch (error) {
      failed = true
      console.error(`DISPATCH_ERROR ${error.stack || error}`)
      break
    }
    await sleep(commandDelay)
  }
  ranAllCommands = true
  await sleep(commandDelay)
  console.log(`SUMMARY ${json({ commands: commands.length, commandOutputs, textPackets, formPackets, failed })}`)
  finish(failed ? 1 : 0)
})

client.on('close', () => {
  console.log('CLOSE')
  if (!ranAllCommands) failed = true
  if (!finished) finish(failed ? 1 : 0)
})

setTimeout(() => {
  console.error('TIMEOUT UMoney E2E did not complete')
  finish(1)
}, Number(process.env.E2E_TIMEOUT_MS || 45000))
