package com.prismdisplay.client

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.MotionEvent
import android.view.Surface as DecoderSurface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowInsets
import android.view.WindowInsetsController
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import kotlinx.coroutines.*
import java.io.DataInputStream
import java.io.DataOutputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.net.ssl.SSLContext
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.toDegrees

data class DiscoveredHost(val name: String, val ip: String, val port: Int, val isUsb: Boolean = false)

class PrismStreamClient {
    private var socket: Socket? = null
    private var outStream: DataOutputStream? = null
    private var decoder: MediaCodec? = null
    private val sendLock = Any()

    @Volatile
    var isConnected = false
        private set

    var latencyMs by mutableStateOf(0L)
        private set

    fun connect(
        host: DiscoveredHost,
        pin: String,
        useTls: Boolean,
        surface: DecoderSurface,
        onDisconnect: (String) -> Unit
    ) {
        CoroutineScope(Dispatchers.IO).launch {
            try {
                val rawSocket = Socket().apply {
                    tcpNoDelay = true
                    receiveBufferSize = 1024 * 1024
                    connect(InetSocketAddress(host.ip, host.port), 3000)
                }

                val activeSocket = if (useTls && !host.isUsb) {
                    val sslContext = SSLContext.getInstance("TLSv1.3")
                    sslContext.init(null, null, null)
                    sslContext.socketFactory.createSocket(
                        rawSocket, host.ip, host.port, true
                    ) as Socket
                } else {
                    rawSocket
                }

                socket = activeSocket
                outStream = DataOutputStream(activeSocket.getOutputStream())
                val inStream = DataInputStream(activeSocket.getInputStream())

                sendPacket(0x01, pin.toByteArray(Charsets.UTF_8), System.nanoTime() / 1000)
                setupDecoder(surface)
                isConnected = true

                launch {
                    while (isConnected) {
                        sendPacket(0x05, ByteArray(0), System.nanoTime() / 1000)
                        delay(1000)
                    }
                }

                val header = ByteArray(16)
                while (isConnected) {
                    inStream.readFully(header)
                    val buffer = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN)
                    val magic = buffer.short.toInt() and 0xFFFF
                    if (magic != MAGIC) throw IllegalStateException("Stream desync")
                    buffer.get()
                    val type = buffer.get().toInt() and 0xFF
                    val length = buffer.int
                    val timestampUs = buffer.long
                    if (length < 0 || length > MAX_PACKET_SIZE) {
                        throw IllegalStateException("Invalid packet length")
                    }

                    val payload = ByteArray(length)
                    if (length > 0) inStream.readFully(payload)

                    when (type) {
                        0x03 -> feedDecoder(payload, timestampUs)
                        0x05 -> latencyMs = ((System.nanoTime() / 1000) - timestampUs) / 1000
                    }
                }
            } catch (e: Exception) {
                isConnected = false
                withContext(Dispatchers.Main) { onDisconnect(e.message ?: "Disconnected") }
            }
        }
    }

    private fun setupDecoder(surface: DecoderSurface) {
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 1920, 1080)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
        }
        decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).apply {
            configure(format, surface, null, 0)
            start()
        }
    }

    private fun feedDecoder(nalData: ByteArray, presentationTimeUs: Long) {
        val codec = decoder ?: return
        val inputIndex = codec.dequeueInputBuffer(5000)
        if (inputIndex >= 0) {
            val inputBuffer = codec.getInputBuffer(inputIndex)
            inputBuffer?.clear()
            inputBuffer?.put(nalData)
            codec.queueInputBuffer(inputIndex, 0, nalData.size, presentationTimeUs, 0)
        }

        val info = MediaCodec.BufferInfo()
        var outputIndex = codec.dequeueOutputBuffer(info, 0)
        while (outputIndex >= 0) {
            codec.releaseOutputBuffer(outputIndex, true)
            outputIndex = codec.dequeueOutputBuffer(info, 0)
        }
    }

    fun sendMotionEvent(event: MotionEvent, viewWidth: Float, viewHeight: Float) {
        if (!isConnected || viewWidth <= 0f || viewHeight <= 0f) return

        val actionCode = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> 0
            MotionEvent.ACTION_MOVE -> 1
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> 2
            MotionEvent.ACTION_HOVER_MOVE, MotionEvent.ACTION_HOVER_ENTER -> 3
            else -> return
        }

        val index = event.actionIndex
        val toolType = when (event.getToolType(index)) {
            MotionEvent.TOOL_TYPE_STYLUS -> 2
            MotionEvent.TOOL_TYPE_ERASER -> 3
            else -> 1
        }
        var buttons = 0
        if ((event.buttonState and MotionEvent.BUTTON_STYLUS_PRIMARY) != 0) buttons = buttons or 0x01
        if (toolType == 3) buttons = buttons or 0x02

        val normalizedX = (event.getX(index) / viewWidth).coerceIn(0f, 1f)
        val normalizedY = (event.getY(index) / viewHeight).coerceIn(0f, 1f)
        val pressure = event.getPressure(index).coerceIn(0f, 1f)
        val tilt = event.getAxisValue(MotionEvent.AXIS_TILT, index).toDouble()
        val orientation = event.getOrientation(index).toDouble()
        val tiltX = toDegrees(tilt * sin(orientation)).toInt().coerceIn(-90, 90).toShort()
        val tiltY = toDegrees(tilt * cos(orientation)).toInt().coerceIn(-90, 90).toShort()

        val buffer = ByteBuffer.allocate(24).order(ByteOrder.LITTLE_ENDIAN).apply {
            put(toolType.toByte())
            put(actionCode.toByte())
            putShort(buttons.toShort())
            putFloat(normalizedX)
            putFloat(normalizedY)
            putFloat(pressure)
            putShort(tiltX)
            putShort(tiltY)
            putInt(event.getPointerId(index))
        }
        CoroutineScope(Dispatchers.IO).launch {
            sendPacket(0x04, buffer.array(), System.nanoTime() / 1000)
        }
    }

    private fun sendPacket(type: Int, payload: ByteArray, timestampUs: Long) {
        synchronized(sendLock) {
            try {
                val header = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN)
                    .putShort(MAGIC.toShort())
                    .put(1.toByte())
                    .put(type.toByte())
                    .putInt(payload.size)
                    .putLong(timestampUs)
                outStream?.write(header.array())
                if (payload.isNotEmpty()) outStream?.write(payload)
                outStream?.flush()
            } catch (_: Exception) {
                isConnected = false
            }
        }
    }

    fun disconnect() {
        isConnected = false
        try {
            decoder?.stop()
            decoder?.release()
        } catch (_: Exception) {
        }
        decoder = null
        try {
            socket?.close()
        } catch (_: Exception) {
        }
        socket = null
    }

    private companion object {
        const val MAGIC = 0x5052
        const val MAX_PACKET_SIZE = 16 * 1024 * 1024
    }
}

class MainActivity : ComponentActivity() {
    private val client = PrismStreamClient()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme(colorScheme = darkColorScheme()) {
                PrismAppScreen(client = client, onEnterImmersive = ::enableImmersiveMode)
            }
        }
    }

    private fun enableImmersiveMode() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                it.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        }
    }

    override fun onDestroy() {
        client.disconnect()
        super.onDestroy()
    }
}

@Composable
fun PrismAppScreen(client: PrismStreamClient, onEnterImmersive: () -> Unit) {
    var activeHost by remember { mutableStateOf<DiscoveredHost?>(null) }
    var manualIp by remember { mutableStateOf("192.168.1.100") }
    var pinCode by remember { mutableStateOf("") }
    var useTls by remember { mutableStateOf(true) }
    var statusBanner by remember { mutableStateOf("Ready to connect via USB or Wi-Fi") }
    val discoveredHosts = remember {
        mutableStateListOf(DiscoveredHost("USB Direct Tunnel (ADB)", "127.0.0.1", 38400, isUsb = true))
    }

    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            val udp = DatagramSocket(38401).apply {
                broadcast = true
                soTimeout = 2000
            }
            try {
                val data = ByteArray(256)
                while (isActive) {
                    try {
                        val packet = DatagramPacket(data, data.size)
                        udp.receive(packet)
                        val message = String(packet.data, 0, packet.length, Charsets.UTF_8)
                        val parts = message.split("|")
                        if (parts.size == 3 && parts[0] == "PRISM_HOST") {
                            val ip = packet.address.hostAddress ?: continue
                            val host = DiscoveredHost(parts[1], ip, parts[2].toInt())
                            withContext(Dispatchers.Main) {
                                if (discoveredHosts.none { it.ip == ip }) discoveredHosts.add(host)
                            }
                        }
                    } catch (_: java.net.SocketTimeoutException) {
                    } catch (_: NumberFormatException) {
                    }
                }
            } finally {
                udp.close()
            }
        }
    }

    if (activeHost != null) {
        Box(Modifier.fillMaxSize().background(Color.Black)) {
            AndroidView(
                modifier = Modifier.fillMaxSize(),
                factory = { context ->
                    SurfaceView(context).apply {
                        setOnTouchListener { view, event ->
                            client.sendMotionEvent(event, view.width.toFloat(), view.height.toFloat())
                            true
                        }
                        setOnHoverListener { view, event ->
                            client.sendMotionEvent(event, view.width.toFloat(), view.height.toFloat())
                            true
                        }
                        holder.addCallback(object : SurfaceHolder.Callback {
                            override fun surfaceCreated(holder: SurfaceHolder) {
                                onEnterImmersive()
                                client.connect(activeHost!!, pinCode, useTls, holder.surface) { error ->
                                    statusBanner = error
                                    activeHost = null
                                }
                            }

                            override fun surfaceChanged(
                                holder: SurfaceHolder, format: Int, width: Int, height: Int
                            ) = Unit

                            override fun surfaceDestroyed(holder: SurfaceHolder) {
                                client.disconnect()
                            }
                        })
                    }
                }
            )
            Surface(
                modifier = Modifier.align(Alignment.TopEnd).padding(12.dp),
                color = Color(0x99111827),
                shape = RoundedCornerShape(16.dp)
            ) {
                Row(
                    modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        "${client.latencyMs} ms",
                        color = Color(0xFF34D399),
                        fontSize = 12.sp,
                        fontWeight = FontWeight.Bold
                    )
                    Spacer(Modifier.width(12.dp))
                    Text(
                        "Disconnect",
                        color = Color(0xFFF87171),
                        fontSize = 12.sp,
                        modifier = Modifier.clickable {
                            client.disconnect()
                            activeHost = null
                        }
                    )
                }
            }
        }
    } else {
        Column(
            modifier = Modifier.fillMaxSize().background(Color(0xFF0F1117)).padding(24.dp)
        ) {
            Text("PrismDisplay", fontSize = 28.sp, fontWeight = FontWeight.Bold, color = Color(0xFF38BDF8))
            Text(
                statusBanner,
                fontSize = 14.sp,
                color = Color(0xFF9CA3AF),
                modifier = Modifier.padding(bottom = 20.dp)
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                OutlinedTextField(
                    value = pinCode,
                    onValueChange = { pinCode = it },
                    label = { Text("Host Pairing PIN (Wi-Fi)") },
                    modifier = Modifier.weight(1f)
                )
                OutlinedTextField(
                    value = manualIp,
                    onValueChange = { manualIp = it },
                    label = { Text("Manual PC IP") },
                    modifier = Modifier.weight(1f)
                )
                Button(
                    onClick = {
                        activeHost = DiscoveredHost("Manual PC", manualIp, 38400)
                    },
                    modifier = Modifier.align(Alignment.CenterVertically).height(54.dp)
                ) {
                    Text("Connect IP")
                }
            }
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.padding(vertical = 12.dp)
            ) {
                Checkbox(checked = useTls, onCheckedChange = { useTls = it })
                Text("Enable TLS 1.3 Wi-Fi Encryption", color = Color.White)
            }
            Text(
                "DISCOVERED COMPUTERS & USB",
                fontSize = 12.sp,
                fontWeight = FontWeight.Bold,
                color = Color(0xFF6B7280),
                modifier = Modifier.padding(top = 12.dp, bottom = 8.dp)
            )
            LazyColumn(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                items(discoveredHosts) { host ->
                    Card(
                        colors = CardDefaults.cardColors(containerColor = Color(0xFF161922)),
                        modifier = Modifier.fillMaxWidth().clickable { activeHost = host }
                    ) {
                        Row(
                            modifier = Modifier.padding(16.dp),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column {
                                Text(host.name, fontWeight = FontWeight.Bold, color = Color.White, fontSize = 16.sp)
                                Text("${host.ip}:${host.port}", color = Color(0xFF9CA3AF), fontSize = 13.sp)
                            }
                            Text(
                                if (host.isUsb) "USB 3.0" else "WI-FI",
                                color = Color(0xFF38BDF8),
                                fontWeight = FontWeight.Bold
                            )
                        }
                    }
                }
            }
        }
    }
}
