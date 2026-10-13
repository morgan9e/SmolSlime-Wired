package dev.smolslime.quest;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.widget.*;

public final class MainActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView status;
    private long previousReports, previousSent;
    private final Runnable refresh = new Runnable() {
        public void run() {
            long reports = ForwardService.reports.get(), sent = ForwardService.sent.get();
            status.setText(ForwardService.state + "\n" + ForwardService.networkState + "\nUSB reports/s: " + (reports - previousReports)
                + "\nDatagrams/s: " + (sent - previousSent) + "\nQueue drops: " + ForwardService.dropped.get()
                + "\nNetwork errors: " + ForwardService.errors.get());
            previousReports = reports; previousSent = sent;
            handler.postDelayed(this, 1000);
        }
    };
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        SharedPreferences prefs = getSharedPreferences("forwarder", MODE_PRIVATE);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(32, 24, 32, 24);
        TextView title = new TextView(this);
        title.setText("Quest Slime Forwarder\nEnter the PC's hostname or IPv4 address, then connect the Nordic receiver.");
        title.setTextSize(22); layout.addView(title);
        EditText host = new EditText(this);
        host.setHint("PC hostname or IPv4 address"); host.setSingleLine(true);
        host.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        host.setText(prefs.getString("host", "")); layout.addView(host);
        EditText port = new EditText(this);
        port.setHint("PC port"); port.setInputType(InputType.TYPE_CLASS_NUMBER);
        port.setText(Integer.toString(prefs.getInt("port", 6970))); layout.addView(port);
        Button start = new Button(this); start.setText("Start forwarding"); layout.addView(start);
        Button stop = new Button(this); stop.setText("Stop"); layout.addView(stop);
        status = new TextView(this); status.setTextSize(18); layout.addView(status);
        start.setOnClickListener(v -> {
            String ip = host.getText().toString().trim();
            int number;
            try {
                number = Integer.parseInt(port.getText().toString());
                if (number < 1 || number > 65535) throw new IllegalArgumentException();
                if (ip.isEmpty() || ip.length() > 253 || !ip.matches("[A-Za-z0-9_.-]+"))
                    throw new IllegalArgumentException();
            } catch (IllegalArgumentException e) {
                Toast.makeText(this, "Enter a hostname or IPv4 address (no URL), and port 1–65535", Toast.LENGTH_LONG).show(); return;
            }
            prefs.edit().putString("host", ip).putInt("port", number).apply();
            startForegroundService(new Intent(this, ForwardService.class));
        });
        stop.setOnClickListener(v -> stopService(new Intent(this, ForwardService.class)));
        setContentView(layout);
    }
    public void onResume() { super.onResume(); handler.post(refresh); }
    public void onPause() { handler.removeCallbacks(refresh); super.onPause(); }
}
