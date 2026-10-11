package dev.smolslime.quest;

import android.app.*;
import android.content.*;
import android.hardware.usb.*;
import android.os.*;
import java.net.*;
import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

public final class ForwardService extends Service {
    public static volatile String state = "Stopped";
    public static volatile String networkState = "";
    public static final AtomicLong reports = new AtomicLong(), sent = new AtomicLong(),
        dropped = new AtomicLong(), errors = new AtomicLong();
    private static final String PERMISSION = "dev.smolslime.quest.USB_PERMISSION";
    private final ArrayBlockingQueue<Frame> queue = new ArrayBlockingQueue<>(16);
    private final SecureRandom random = new SecureRandom();
    private volatile boolean running;
    private volatile Link link = new Link(0, false, "");
    private volatile UsbDeviceConnection connection;
    private volatile DatagramSocket socket;
    private Thread usbThread, networkThread;
    private PowerManager.WakeLock wake;
    private UsbManager manager;
    private String host;
    private int port;
    private final BroadcastReceiver permission = new BroadcastReceiver() {
        public void onReceive(Context context, Intent intent) {
            if (PERMISSION.equals(intent.getAction()) &&
                !intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false))
                state = "USB permission denied. Stop and start to retry.";
        }
    };
    private static final class Link {
        final long session; final boolean connected; final String serial;
        Link(long session, boolean connected, String serial) {
            this.session = session; this.connected = connected; this.serial = serial;
        }
    }
    private static final class Frame {
        final long session, time; final byte[] data;
        Frame(long session, byte[] data) { this.session = session; this.data = data; time = SystemClock.elapsedRealtime(); }
    }
    public IBinder onBind(Intent intent) { return null; }
    public void onCreate() {
        super.onCreate();
        manager = (UsbManager)getSystemService(USB_SERVICE);
        registerReceiver(permission, new IntentFilter(PERMISSION));
        NotificationManager nm = (NotificationManager)getSystemService(NOTIFICATION_SERVICE);
        nm.createNotificationChannel(new NotificationChannel("forwarding", "Receiver forwarding", NotificationManager.IMPORTANCE_LOW));
        PendingIntent open = PendingIntent.getActivity(this, 0, new Intent(this, MainActivity.class), PendingIntent.FLAG_IMMUTABLE);
        startForeground(1, new Notification.Builder(this, "forwarding")
            .setContentTitle("Quest Slime Forwarder").setContentText("USB receiver forwarding is active")
            .setSmallIcon(android.R.drawable.stat_sys_upload).setContentIntent(open).setOngoing(true).build());
    }
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (running) { state = "Already running. Stop before changing the PC address."; return START_NOT_STICKY; }
        SharedPreferences prefs = getSharedPreferences("forwarder", MODE_PRIVATE);
        host = prefs.getString("host", ""); port = prefs.getInt("port", 6970);
        if (host.isEmpty()) { stopSelf(); return START_NOT_STICKY; }
        reports.set(0); sent.set(0); dropped.set(0); errors.set(0);
        running = true;
        networkState = "Resolving " + host;
        link = new Link(random.nextLong(), false, "");
        wake = ((PowerManager)getSystemService(POWER_SERVICE)).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "QuestSlime:forwarder");
        wake.acquire();
        networkThread = new Thread(this::networkLoop, "QuestSlime-UDP");
        usbThread = new Thread(this::usbLoop, "QuestSlime-USB");
        networkThread.start(); usbThread.start();
        return START_NOT_STICKY;
    }
    private void pause() throws InterruptedException { Thread.sleep(500); }
    private void usbLoop() {
        String requested = null;
        try {
            while (running) {
                UsbDevice device = null;
                for (UsbDevice candidate : manager.getDeviceList().values())
                    if (candidate.getVendorId() == 0x1209 && candidate.getProductId() == 0x7690) { device = candidate; break; }
                if (device == null) { requested = null; state = "Waiting for Nordic receiver"; pause(); continue; }
                if (!manager.hasPermission(device)) {
                    if (!device.getDeviceName().equals(requested)) {
                        requested = device.getDeviceName(); state = "Allow USB access in the headset";
                        Intent request = new Intent(PERMISSION).setPackage(getPackageName());
                        manager.requestPermission(device, PendingIntent.getBroadcast(this, 0, request, PendingIntent.FLAG_IMMUTABLE));
                    }
                    pause(); continue;
                }
                UsbInterface iface = null; UsbEndpoint endpoint = null;
                for (int i = 0; i < device.getInterfaceCount() && endpoint == null; i++) {
                    UsbInterface candidate = device.getInterface(i);
                    if (candidate.getInterfaceClass() != UsbConstants.USB_CLASS_HID) continue;
                    for (int j = 0; j < candidate.getEndpointCount(); j++) {
                        UsbEndpoint ep = candidate.getEndpoint(j);
                        if (ep.getDirection() == UsbConstants.USB_DIR_IN && ep.getType() == UsbConstants.USB_ENDPOINT_XFER_INT && ep.getMaxPacketSize() == 64) {
                            iface = candidate; endpoint = ep; break;
                        }
                    }
                }
                if (endpoint == null) { state = "Receiver has no compatible HID endpoint"; pause(); continue; }
                UsbDeviceConnection conn = manager.openDevice(device);
                if (conn == null) { state = "Cannot open receiver"; pause(); continue; }
                connection = conn;
                UsbRequest request = new UsbRequest();
                boolean claimed = false;
                try {
                    claimed = conn.claimInterface(iface, true);
                    if (!claimed || !request.initialize(conn, endpoint)) throw new IllegalStateException("Cannot claim HID interface");
                    queue.clear();
                    String serial = device.getSerialNumber();
                    Link active = new Link(random.nextLong(), true, serial == null ? "unknown" : serial);
                    link = active; state = "Forwarding to " + host + ":" + port;
                    ByteBuffer buffer = ByteBuffer.allocateDirect(64);
                    boolean pending = false;
                    while (running && manager.getDeviceList().containsKey(device.getDeviceName())) {
                        if (!pending) {
                            buffer.clear();
                            if (!request.queue(buffer)) throw new IllegalStateException("USB queue failed");
                            pending = true;
                        }
                        UsbRequest completed;
                        try { completed = conn.requestWait(500); }
                        catch (TimeoutException timeout) { continue; } // Keep the outstanding request queued.
                        if (completed != request) throw new IllegalStateException("USB read failed");
                        pending = false;
                        if (buffer.position() != 64) { dropped.incrementAndGet(); continue; }
                        byte[] data = new byte[64]; buffer.flip(); buffer.get(data);
                        reports.incrementAndGet();
                        Frame frame = new Frame(active.session, data);
                        if (!queue.offer(frame)) { queue.poll(); dropped.incrementAndGet(); queue.offer(frame); }
                    }
                } catch (RuntimeException e) {
                    if (running) state = "USB reconnecting: " + e.getMessage();
                } finally {
                    request.cancel(); request.close();
                    if (claimed) conn.releaseInterface(iface);
                    conn.close(); connection = null;
                    requested = null;
                    link = new Link(random.nextLong(), false, ""); queue.clear();
                }
                if (running) pause();
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (RuntimeException e) {
            state = "USB stopped: " + e.getMessage(); stopSelf();
        }
    }
    private void send(DatagramSocket udp, int kind, Link current, int sequence, byte[] payload) throws java.io.IOException {
        ByteBuffer packet = ByteBuffer.allocate(20 + payload.length).order(ByteOrder.LITTLE_ENDIAN);
        packet.put(new byte[]{'S','N','R','2'}).put((byte)kind).put((byte)(current.connected ? 1 : 0));
        packet.putShort((short)payload.length).putLong(current.session).putInt(sequence).put(payload);
        udp.send(new DatagramPacket(packet.array(), packet.position())); sent.incrementAndGet();
    }
    private void networkLoop() {
        try (DatagramSocket udp = new DatagramSocket()) {
            socket = udp;
            InetAddress destination = null;
            for (InetAddress address : InetAddress.getAllByName(host)) {
                if (address instanceof Inet4Address) { destination = address; break; }
            }
            if (destination == null) throw new UnknownHostException("No IPv4 address for " + host);
            if (!running) return;
            udp.connect(destination, port);
            networkState = host + " → " + destination.getHostAddress() + ":" + port;
            long session = 0, helloAt = 0; int sequence = 0;
            while (running) {
                Frame frame = queue.poll(100, TimeUnit.MILLISECONDS);
                Link current = link;
                long now = SystemClock.elapsedRealtime();
                try {
                    if (current.session != session || now - helloAt >= 500) {
                        if (current.session != session) { session = current.session; sequence = 0; }
                        byte[] serial = current.serial.getBytes(StandardCharsets.UTF_8);
                        if (serial.length > 128) serial = java.util.Arrays.copyOf(serial, 128);
                        send(udp, 2, current, sequence++, serial); helloAt = now;
                    }
                    if (frame != null) {
                        if (frame.session == current.session && current.connected && now - frame.time < 100)
                            send(udp, 1, current, sequence++, frame.data);
                        else dropped.incrementAndGet();
                    }
                } catch (java.io.IOException e) { errors.incrementAndGet(); }
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (java.io.IOException e) {
            networkState = "Network stopped: " + e.getMessage(); stopSelf();
        } finally { socket = null; }
    }
    public void onDestroy() {
        running = false;
        if (usbThread != null) usbThread.interrupt();
        if (networkThread != null) networkThread.interrupt();
        // requestWait has a 500ms timeout, so the USB owner closes its connection.
        DatagramSocket udp = socket; if (udp != null) udp.close();
        if (wake != null && wake.isHeld()) wake.release();
        unregisterReceiver(permission); stopForeground(true);
        state = "Stopped"; super.onDestroy();
    }
}
