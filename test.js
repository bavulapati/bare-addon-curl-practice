const test = require('brittle')
const { tcpConnect } = require('.')

test('validate function signature', async (t) => {
  t.is(typeof tcpConnect, 'function', 'default export should be a function')
  t.exception.all(() => {
    tcpConnect()
  }, 'missing arguments')
  t.exception.all(() => {
    tcpConnect('12')
  }, 'missing arguments')
  t.exception.all(() => {
    tcpConnect(4242)
  }, 'missing arguments')
  t.exception(() => {
    tcpConnect('127.0.0.', 4242, 'hello\r\n')
  }, 'expects a valid host')
  t.exception.all(() => {
    tcpConnect(127, '4242', 'hello\r\n')
  }, 'expects host as string')
  t.exception.all(() => {
    tcpConnect('127.0.0.1', '4242', 'hello\r\n')
  }, 'expects port as number')
  t.exception.all(() => {
    tcpConnect('127.0.0.1', '4242', 619)
  }, 'expects message as string')
  t.exception.all(() => {
    tcpConnect('127.0.0.1', 4242, 619)
  }, 'expects message as string')
  await t.exception(async () => {
    await tcpConnect('127.0.0.1', 4241, 'hello\r\n\r\n')
  })
})

function waitForServer(server) {
  return new Promise((resolve, reject) => {
    function done(error) {
      error ? reject(error) : resolve()
    }

    server.on('listening', done).on('error', done)
  })
}

test('should receive data when server is accepting connections', async (t) => {
  const tcp = require('bare-tcp')
  const server = tcp.createServer()
  server.on('connection', (c) => {
    c.on('data', (data) => {
      console.log('server received: ', data.toString())
    })
    c.on('close', () => {
      console.log('server connection closed')
    })
    c.end('hello\r\n')
  })
  server.on('error', (err) => {
    t.fail('server error')
  })
  server.listen(4241, '127.0.0.1', () => {
    console.log('server bound')
  })

  await waitForServer(server)

  const { port } = server.address()
  console.log('port: ', port)
  try {
    const msg = await tcpConnect('127.0.0.1', port, 'hello\r\n')
    console.log('msg: ', msg)
    server.close()
  } catch (err) {
    console.error(err)
  }
  // await t.execution(tcpConnect('127.0.0.1', port, 'hello\r\n'))
})
test('remote server', async (t) => {
  await t.execution(async () => {
    const promise = tcpConnect(
      '23.192.228.80',
      80,
      'GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n'
    )
    t.is(typeof promise, 'object')
    const value = await promise
    t.is(typeof value, 'string')
    console.log('received: ', value)
  })
})

test('multiple connections', async (t) => {
  const tcp = require('bare-tcp')
  const server = tcp.createServer()
  server.on('connection', (c) => {
    c.on('data', (data) => {
      console.log('server received: ', data.toString())
    })
    c.on('close', () => {
      console.log('server connection closed')
    })
    c.end('hello\r\n')
  })
  server.on('error', (err) => {
    t.fail('server error')
  })
  server.listen(4243, '127.0.0.1', () => {
    console.log('server bound')
  })

  await t.execution(async () => {
    await waitForServer(server)
    const client = tcpConnect('127.0.0.1', 4243, 'hello\r\n')

    const client2 = tcpConnect('127.0.0.1', 4243, 'hello\r\n')
    t.is(typeof client, 'object')
    const value = await client
    t.is(typeof value, 'string')
    console.log('received: ', value)

    t.is(typeof client2, 'object')
    const value2 = await client2
    t.is(typeof value2, 'string')
    console.log('received: ', value2)
  })
  server.close()
})
test('Mixed connections', async (t) => {
  await t.execution(async () => {
    const promise = tcpConnect(
      '23.192.228.80',
      80,
      'GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n'
    )
    t.is(typeof promise, 'object')
    const value = await promise
    t.is(typeof value, 'string')
    console.log('received: ', value)
  })
  await t.exception(async () => {
    await tcpConnect('127.0.0.1', 4241, 'hello\r\n\r\n')
  })
})
