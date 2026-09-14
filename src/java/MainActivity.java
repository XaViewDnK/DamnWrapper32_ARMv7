package com.damnwrapper32armv7.xaview;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.nio.ByteBuffer;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import android.widget.GridView;
import android.widget.ProgressBar;
import android.widget.LinearLayout;
import android.widget.ImageView;
import android.widget.BaseAdapter;
import android.view.ViewGroup;
import android.graphics.drawable.GradientDrawable;
import android.graphics.BitmapFactory;
import android.graphics.Bitmap;
import android.graphics.Typeface;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.content.Context;

public class MainActivity extends Activity implements SurfaceHolder.Callback, SensorEventListener {

    static {
        System.loadLibrary("DamnWrapper32");
    }

    private TextView logTextView;
    private ScrollView scrollView;
    private FrameLayout rootLayout;
    private SurfaceView surfaceView;
    private volatile boolean isRendering = false;

    private LinearLayout unpackLayout;
    private ProgressBar unpackProgress;
    private TextView unpackText;
    private TextView unpackAppName;
    private TextView unpackPkgName;
    private TextView unpackVersion;

    private LinearLayout deleteLayout;
    private ProgressBar deleteProgress;
    private TextView deleteText;
    private TextView deleteAppName;
    private TextView deletePkgName;
    private TextView deleteVersion;
    private GridView launcherGrid;
    private TextView noGamesText;
    private android.widget.Button aboutButton;
    private android.widget.Button settingsButton;
    private LinearLayout bottomButtonsLayout;
    private LinearLayout aboutLayout;
    private LinearLayout settingsLayout;
    private LinearLayout commandsLayout;
    private LinearLayout loggingFiltersLayout;
    private LinearLayout loggingSpamFiltersLayout;
    private LinearLayout gpuOffloadLayout;
    private android.widget.CheckBox onScreenDebugOverlayCheckbox;
    private android.widget.CheckBox showPerfOverlayCheckbox;
    private android.widget.CheckBox nativeRootMmapCheckbox;
    private android.widget.Button esModeButton;
    private List<AppInfo> installedApps = new ArrayList<>();

    private float scaleFactorX = 1f;
    private float scaleFactorY = 1f;

    private SensorManager sensorManager;
    private Sensor accelerometer;
    private Sensor gyroscope;
    private boolean isSetupStarted = false;

    // --- ВАРИАНТЫ ВИДЕОПЛЕЕРА ---
    private FrameLayout videoContainer;
    private android.widget.VideoView videoView;
    private android.widget.Button skipButton;
    private android.os.Handler videoHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private Runnable hideSkipRunnable;
    private int activeVideoPtrId = 0;

    // Хранилище аудиоплееров: C++ Pointer ID -> Android MediaPlayer
    private HashMap<Integer, MediaPlayer> audioPlayers = new HashMap<>();
    private AudioTrack streamTrack;

    public void audioUnitStreamInit(int sampleRate, int channels) {
        if (streamTrack != null) {
            streamTrack.stop();
            streamTrack.release();
        }
        int channelConfig = (channels == 2) ? AudioFormat.CHANNEL_OUT_STEREO : AudioFormat.CHANNEL_OUT_MONO;
        int minSize = AudioTrack.getMinBufferSize(sampleRate, channelConfig, AudioFormat.ENCODING_PCM_16BIT);
        streamTrack = new AudioTrack(AudioManager.STREAM_MUSIC, sampleRate, channelConfig, AudioFormat.ENCODING_PCM_16BIT, minSize * 4, AudioTrack.MODE_STREAM);
        streamTrack.play();
    }

    public void audioUnitStreamWrite(byte[] data, int size) {
        if (streamTrack != null) {
            streamTrack.write(data, 0, size);
        }
    }

    // --- OPENAL СТЕЙТ ---
    private HashMap<Integer, byte[]> alBuffers = new HashMap<>();
    private HashMap<Integer, Integer> alBufferFreqs = new HashMap<>();
    private HashMap<Integer, Integer> alBufferChannels = new HashMap<>();
    private HashMap<Integer, Integer> alSourceToBuffer = new HashMap<>();
    private HashMap<Integer, AudioTrack> alSourceTracks = new HashMap<>();

    private static final String WORK_DIR = Environment.getExternalStorageDirectory() + "/DamnWrapper32_ARMv7/";
    private static final String APPS_DIR = WORK_DIR + "apps/";
    private static final String SETUP_DIR = WORK_DIR + "setup/";

    // Порядок задаёт номера битов в маске и должен совпадать с enum LogCat в main.cpp.
    static final String[] LOG_CAT_KEYS = {"log_render", "log_render_dump", "log_sound", "log_fs", "log_mem",
                                          "log_objc", "log_net", "log_todo", "log_diag", "log_game", "log_other"};
    static final String[] LOG_SPAM_KEYS = {"spam_log_render", "spam_log_render_dump", "spam_log_sound", "spam_log_fs", "spam_log_mem",
                                           "spam_log_objc", "spam_log_net", "spam_log_todo", "spam_log_diag", "spam_log_game", "spam_log_other"};
    static final String[] LOG_CAT_NAMES = {"Render: GL, EGL, шейдеры", "Render dump: покадровый снимок GL-состояния",
                                           "Sound: аудио и OpenAL", "File system: файловые операции",
                                           "Memory: malloc/free гостя", "ObjC: вызовы сообщений",
                                           "Network, Bluetooth, GPS", "TODO: заглушки и нереализованное",
                                           "Diag: списки функций и классов", "Game: вывод самой игры (printf, NSLog)",
                                           "Other: всё остальное"};

    private int buildLogMask() {
        android.content.SharedPreferences prefs = getSharedPreferences("DamnPrefs", MODE_PRIVATE);
        int m = 0;
        for (int j = 0; j < LOG_CAT_KEYS.length; j++) if (prefs.getBoolean(LOG_CAT_KEYS[j], false)) m |= (1 << j);
        return m;
    }

    private int buildSpamMask() {
        android.content.SharedPreferences prefs = getSharedPreferences("DamnPrefs", MODE_PRIVATE);
        int m = 0;
        for (int j = 0; j < LOG_SPAM_KEYS.length; j++) if (prefs.getBoolean(LOG_SPAM_KEYS[j], true)) m |= (1 << j);
        return m;
    }

    public native void initWrapper(String workDir, String sandboxRoot, String appBundlePath, String bundleId, int logMask, int spamMask, boolean onScreenDebugOverlay, boolean showPerfOverlay, boolean nativeRootMmap, int resWidth, int resHeight, int esMode, int gpuOffloadMask);
    public native void setLogUiVisible(boolean visible);
    public native void onSurfaceCreated(android.view.Surface surface);
    public native void onSurfaceChanged(int width, int height);
    public native void onTouchEventNative(int actionMasked, int pointerId, float x, float y);
    public native void onSensorChangedNative(int sensorType, float x, float y, float z);

    public native void onVideoFinishedNative(int ptrId);


    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        if (sensorManager != null) {
            accelerometer = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            gyroscope = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        }

        requestWindowFeature(Window.FEATURE_NO_TITLE);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);

        rootLayout = new FrameLayout(this);
        rootLayout.setBackgroundColor(Color.BLACK);

        scrollView = new ScrollView(this);
        logTextView = new TextView(this);
        logTextView.setTextColor(Color.GREEN);
        logTextView.setTextSize(12f);
        logTextView.setTextIsSelectable(true);
        logTextView.setPadding(16, 16, 16, 16);

        scrollView.addView(logTextView);
        scrollView.setVisibility(View.GONE); // Скрываем логи изначально

        // UI Распаковки
        unpackLayout = new LinearLayout(this);
        unpackLayout.setOrientation(LinearLayout.VERTICAL);
        unpackLayout.setGravity(Gravity.CENTER);
        unpackLayout.setVisibility(View.GONE);
        
        unpackProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        unpackProgress.setLayoutParams(new LinearLayout.LayoutParams(600, 50));
        unpackText = new TextView(this);
        unpackText.setTextColor(Color.WHITE);
        unpackText.setPadding(0, 20, 0, 0);
        unpackAppName = new TextView(this);
        unpackAppName.setTextColor(Color.LTGRAY);
        unpackPkgName = new TextView(this);
        unpackPkgName.setTextColor(Color.LTGRAY);
        unpackVersion = new TextView(this);
        unpackVersion.setTextColor(Color.LTGRAY);
        unpackLayout.addView(unpackProgress);
        unpackLayout.addView(unpackText);
        unpackLayout.addView(unpackAppName);
        unpackLayout.addView(unpackPkgName);
        unpackLayout.addView(unpackVersion);

        // UI Удаления
        deleteLayout = new LinearLayout(this);
        deleteLayout.setOrientation(LinearLayout.VERTICAL);
        deleteLayout.setGravity(Gravity.CENTER);
        deleteLayout.setBackgroundColor(Color.parseColor("#CC000000"));
        deleteLayout.setVisibility(View.GONE);
        deleteLayout.setClickable(true);
        
        deleteProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        deleteProgress.setLayoutParams(new LinearLayout.LayoutParams(600, 50));
        deleteText = new TextView(this);
        deleteText.setTextColor(Color.WHITE);
        deleteText.setPadding(0, 20, 0, 0);
        deleteAppName = new TextView(this);
        deleteAppName.setTextColor(Color.LTGRAY);
        deletePkgName = new TextView(this);
        deletePkgName.setTextColor(Color.LTGRAY);
        deleteVersion = new TextView(this);
        deleteVersion.setTextColor(Color.LTGRAY);
        deleteLayout.addView(deleteProgress);
        deleteLayout.addView(deleteText);
        deleteLayout.addView(deleteAppName);
        deleteLayout.addView(deletePkgName);
        deleteLayout.addView(deleteVersion);

        // UI Лаунчера
        launcherGrid = new GridView(this);
        launcherGrid.setNumColumns(4);
        launcherGrid.setHorizontalSpacing(20);
        launcherGrid.setVerticalSpacing(40);
        launcherGrid.setPadding(40, 100, 40, 40);
        launcherGrid.setVisibility(View.GONE);

        noGamesText = new TextView(this);
        noGamesText.setText("Games not finded, put it in DamnWrapper32_ARMv7/apps");
        noGamesText.setTextColor(Color.LTGRAY);
        noGamesText.setTextSize(18f);
        noGamesText.setGravity(Gravity.CENTER);
        noGamesText.setVisibility(View.GONE);

        bottomButtonsLayout = new LinearLayout(this);
        bottomButtonsLayout.setOrientation(LinearLayout.HORIZONTAL);
        bottomButtonsLayout.setVisibility(View.GONE); // Скрываем по умолчанию (до конца сканирования)

        FrameLayout.LayoutParams bottomParams = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        bottomParams.gravity = Gravity.BOTTOM;
        bottomParams.setMargins(40, 0, 40, 100); // Отступы от краев экрана

        LinearLayout.LayoutParams btnLeftParams = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        btnLeftParams.setMargins(0, 0, 20, 0); // Отступ справа от About (между кнопками)

        LinearLayout.LayoutParams btnRightParams = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
        btnRightParams.setMargins(20, 0, 0, 0); // Отступ слева от Settings (между кнопками)

        aboutButton = new android.widget.Button(this);
        aboutButton.setText("About");
        settingsButton = new android.widget.Button(this);
        settingsButton.setText("Settings");

        bottomButtonsLayout.addView(aboutButton, btnLeftParams);
        bottomButtonsLayout.addView(settingsButton, btnRightParams);

        aboutLayout = new LinearLayout(this);
        aboutLayout.setOrientation(LinearLayout.VERTICAL);
        aboutLayout.setGravity(Gravity.CENTER);
        aboutLayout.setBackgroundColor(Color.BLACK);
        aboutLayout.setVisibility(View.GONE);

        TextView aboutText = new TextView(this);
        aboutText.setTextColor(Color.WHITE);
        aboutText.setGravity(Gravity.CENTER);
        aboutText.setMovementMethod(android.text.method.LinkMovementMethod.getInstance());
        String aboutHtml = "DamnWrapper32 (ARMv7) - IOS wrapper by XaView<br><br>" +
                "Current IOS support:<br>" +
                "ARMv7 Only<br>" +
                "OpenGL ES 2.0 Only<br>" +
                "IOS 4.0 maximum in theory at this moment, but in fact IOS 3.1.3-3.2 have better support I guess<br><br>" +
                "If you want support me you can donate me Telegram stars:<br>" +
                "<a href=\"https://t.me/xaviewdnk\">https://t.me/xaviewdnk</a><br><br>" +
                "You can also support me via Steam, by giving gift or trade:<br>" +
                "<a href=\"https://steamcommunity.com/profiles/76561198886703080\">https://steamcommunity.com/profiles/76561198886703080</a>";
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
            aboutText.setText(android.text.Html.fromHtml(aboutHtml, android.text.Html.FROM_HTML_MODE_LEGACY));
        } else {
            aboutText.setText(android.text.Html.fromHtml(aboutHtml));
        }
        
        android.widget.Button aboutBackButton = new android.widget.Button(this);
        aboutBackButton.setText("Back");
        LinearLayout.LayoutParams backParams = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        backParams.setMargins(0, 50, 0, 0);
        
        android.widget.Button commandsListButton = new android.widget.Button(this);
        commandsListButton.setText("Commands list");
        LinearLayout.LayoutParams cmdBtnParams = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        cmdBtnParams.setMargins(0, 20, 0, 0);

        aboutLayout.addView(aboutText);
        aboutLayout.addView(commandsListButton, cmdBtnParams);
        aboutLayout.addView(aboutBackButton, backParams);

        commandsLayout = new LinearLayout(this);
        commandsLayout.setOrientation(LinearLayout.VERTICAL);
        commandsLayout.setGravity(Gravity.CENTER);
        commandsLayout.setBackgroundColor(Color.BLACK);
        commandsLayout.setVisibility(View.GONE);

        TextView commandsText = new TextView(this);
        commandsText.setTextColor(Color.WHITE);
        commandsText.setBackgroundColor(Color.TRANSPARENT);
        commandsText.setGravity(Gravity.CENTER);
        commandsText.setTextIsSelectable(true);
        commandsText.setPadding(40, 40, 40, 40);
        commandsText.setText("-launch packagename_version\n*Example: -launch com.sega.smb2_2.0.0\n#Skips wrapper menus and launch app directly.\n\n-novideo\n#Skips videos in all games");

        android.widget.Button commandsBackButton = new android.widget.Button(this);
        commandsBackButton.setText("Back");

        commandsLayout.addView(commandsText);
        commandsLayout.addView(commandsBackButton, backParams);

        commandsListButton.setOnClickListener(v -> {
            aboutLayout.setVisibility(View.GONE);
            commandsLayout.setVisibility(View.VISIBLE);
        });

        commandsBackButton.setOnClickListener(v -> {
            commandsLayout.setVisibility(View.GONE);
            aboutLayout.setVisibility(View.VISIBLE);
        });

        settingsLayout = new LinearLayout(this);
        settingsLayout.setOrientation(LinearLayout.VERTICAL);
        settingsLayout.setGravity(Gravity.CENTER);
        settingsLayout.setBackgroundColor(Color.BLACK);
        settingsLayout.setVisibility(View.GONE);

        android.widget.Button loggingFiltersButton = new android.widget.Button(this);
        loggingFiltersButton.setText("Logging filters");

        android.widget.Button loggingSpamFiltersButton = new android.widget.Button(this);
        loggingSpamFiltersButton.setText("Logging spam hide filters");

        loggingFiltersLayout = new LinearLayout(this);
        loggingFiltersLayout.setOrientation(LinearLayout.VERTICAL);
        loggingFiltersLayout.setGravity(Gravity.CENTER);
        loggingFiltersLayout.setBackgroundColor(Color.BLACK);
        loggingFiltersLayout.setVisibility(View.GONE);

        String[] filterNames = LOG_CAT_NAMES;
        String[] filterKeys = LOG_CAT_KEYS;

        for (int i = 0; i < filterNames.length; i++) {
            android.widget.CheckBox cb = new android.widget.CheckBox(this);
            cb.setText(filterNames[i]);
            cb.setTextColor(Color.WHITE);
            cb.setChecked(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean(filterKeys[i], false));
            final String key = filterKeys[i];
            cb.setOnCheckedChangeListener((btnView, isChecked) -> {
                getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putBoolean(key, isChecked).apply();
            });
            loggingFiltersLayout.addView(cb);
        }

        android.widget.Button loggingFiltersBackButton = new android.widget.Button(this);
        loggingFiltersBackButton.setText("Back");
        loggingFiltersLayout.addView(loggingFiltersBackButton, backParams);

        loggingFiltersButton.setOnClickListener(v -> {
            settingsLayout.setVisibility(View.GONE);
            loggingFiltersLayout.setVisibility(View.VISIBLE);
        });

        loggingFiltersBackButton.setOnClickListener(v -> {
            loggingFiltersLayout.setVisibility(View.GONE);
            settingsLayout.setVisibility(View.VISIBLE);
        });

        loggingSpamFiltersLayout = new LinearLayout(this);
        loggingSpamFiltersLayout.setOrientation(LinearLayout.VERTICAL);
        loggingSpamFiltersLayout.setGravity(Gravity.CENTER);
        loggingSpamFiltersLayout.setBackgroundColor(Color.BLACK);
        loggingSpamFiltersLayout.setVisibility(View.GONE);

        String[] spamFilterKeys = LOG_SPAM_KEYS;

        for (int i = 0; i < filterNames.length; i++) {
            android.widget.CheckBox cb = new android.widget.CheckBox(this);
            cb.setText(filterNames[i]);
            cb.setTextColor(Color.WHITE);
            cb.setChecked(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean(spamFilterKeys[i], true));
            final String key = spamFilterKeys[i];
            cb.setOnCheckedChangeListener((btnView, isChecked) -> {
                getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putBoolean(key, isChecked).apply();
            });
            loggingSpamFiltersLayout.addView(cb);
        }

        android.widget.Button loggingSpamFiltersBackButton = new android.widget.Button(this);
        loggingSpamFiltersBackButton.setText("Back");
        loggingSpamFiltersLayout.addView(loggingSpamFiltersBackButton, backParams);

        loggingSpamFiltersButton.setOnClickListener(v -> {
            settingsLayout.setVisibility(View.GONE);
            loggingSpamFiltersLayout.setVisibility(View.VISIBLE);
        });

        loggingSpamFiltersBackButton.setOnClickListener(v -> {
            loggingSpamFiltersLayout.setVisibility(View.GONE);
            settingsLayout.setVisibility(View.VISIBLE);
        });

        android.widget.Button gpuOffloadButton = new android.widget.Button(this);
        gpuOffloadButton.setText("GPU Offload Menu (Experimental)");
        
        gpuOffloadLayout = new LinearLayout(this);
        gpuOffloadLayout.setOrientation(LinearLayout.VERTICAL);
        gpuOffloadLayout.setGravity(Gravity.CENTER);
        gpuOffloadLayout.setBackgroundColor(Color.BLACK);
        gpuOffloadLayout.setVisibility(View.GONE);

        String[] gpuNames = {
            "1. EGL Context & SwapBuffers (Bit 0)", 
            "2. glClear (Bit 1)", 
            "3. Shaders & Uniforms (Bit 2)", 
            "4. Textures (Bit 3)", 
            "5. VBO, Attribs & glDraw (Bit 4)", 
            "6. States (Blend, Depth, Cull) (Bit 5)", 
            "7. FBO & Viewport (Bit 6)"
        };

        for (int i = 0; i < gpuNames.length; i++) {
            android.widget.CheckBox cb = new android.widget.CheckBox(this);
            cb.setText(gpuNames[i]);
            cb.setTextColor(Color.WHITE);
            cb.setChecked(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean("gpu_bit_" + i, true));
            final int bitIdx = i;
            cb.setOnCheckedChangeListener((btnView, isChecked) -> {
                getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putBoolean("gpu_bit_" + bitIdx, isChecked).apply();
            });
            gpuOffloadLayout.addView(cb);
        }

        android.widget.Button gpuOffloadBackButton = new android.widget.Button(this);
        gpuOffloadBackButton.setText("Back");
        gpuOffloadLayout.addView(gpuOffloadBackButton, backParams);

        gpuOffloadButton.setOnClickListener(v -> {
            settingsLayout.setVisibility(View.GONE);
            gpuOffloadLayout.setVisibility(View.VISIBLE);
        });

        gpuOffloadBackButton.setOnClickListener(v -> {
            gpuOffloadLayout.setVisibility(View.GONE);
            settingsLayout.setVisibility(View.VISIBLE);
        });

        onScreenDebugOverlayCheckbox = new android.widget.CheckBox(this);
        onScreenDebugOverlayCheckbox.setText("OnScreen debug overlay");
        onScreenDebugOverlayCheckbox.setTextColor(Color.WHITE);
        onScreenDebugOverlayCheckbox.setChecked(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean("onscreen_debug_overlay", false));
        onScreenDebugOverlayCheckbox.setOnCheckedChangeListener((btnView, isChecked) -> {
            getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putBoolean("onscreen_debug_overlay", isChecked).apply();
        });

        showPerfOverlayCheckbox = new android.widget.CheckBox(this);
        showPerfOverlayCheckbox.setText("Show performance overlay (FPS)");
        showPerfOverlayCheckbox.setTextColor(Color.WHITE);
        showPerfOverlayCheckbox.setChecked(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean("show_perf_overlay", false));
        showPerfOverlayCheckbox.setOnCheckedChangeListener((btnView, isChecked) -> {
            getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putBoolean("show_perf_overlay", isChecked).apply();
        });

        nativeRootMmapCheckbox = new android.widget.CheckBox(this);
        nativeRootMmapCheckbox.setText("Native ROOT mmap (Better compatability but need root access)");
        nativeRootMmapCheckbox.setTextColor(Color.RED);
        nativeRootMmapCheckbox.setChecked(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean("native_root_mmap", false));
        nativeRootMmapCheckbox.setOnCheckedChangeListener((btnView, isChecked) -> {
            getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putBoolean("native_root_mmap", isChecked).apply();
        });

        esModeButton = new android.widget.Button(this);
        int currentEsMode = getSharedPreferences("DamnPrefs", MODE_PRIVATE).getInt("es_mode", 2);
        esModeButton.setText("OpenGL ES Mode: " + (currentEsMode == 2 ? "2.0 ✅" : "1.1 ✅"));
        esModeButton.setOnClickListener(v_es -> {
            int mode = getSharedPreferences("DamnPrefs", MODE_PRIVATE).getInt("es_mode", 2);
            int newMode = mode == 2 ? 1 : 2;
            getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putInt("es_mode", newMode).apply();
            esModeButton.setText("OpenGL ES Mode: " + (newMode == 2 ? "2.0 ✅" : "1.1 ✅"));
        });

        LinearLayout resLayout = new LinearLayout(this);
        resLayout.setOrientation(LinearLayout.HORIZONTAL);
        resLayout.setGravity(Gravity.CENTER);
        resLayout.setPadding(0, 20, 0, 20);

        android.widget.EditText widthInput = new android.widget.EditText(this);
        widthInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        widthInput.setTextColor(Color.WHITE);
        widthInput.setHintTextColor(Color.GRAY);
        widthInput.setText(String.valueOf(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getInt("res_width", 480)));
        widthInput.setEms(4);

        TextView xText = new TextView(this);
        xText.setText(" x ");
        xText.setTextColor(Color.WHITE);
        xText.setTextSize(16f);

        android.widget.EditText heightInput = new android.widget.EditText(this);
        heightInput.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        heightInput.setTextColor(Color.WHITE);
        heightInput.setHintTextColor(Color.GRAY);
        heightInput.setText(String.valueOf(getSharedPreferences("DamnPrefs", MODE_PRIVATE).getInt("res_height", 320)));
        heightInput.setEms(4);

        android.widget.Button resResetButton = new android.widget.Button(this);
        resResetButton.setText("Reset");
        resResetButton.setOnClickListener(v_res -> {
            widthInput.setText("480");
            heightInput.setText("320");
        });

        android.widget.Button resApplyButton = new android.widget.Button(this);
        resApplyButton.setText("Apply");
        resApplyButton.setOnClickListener(v_res -> {
            try {
                int w = Integer.parseInt(widthInput.getText().toString());
                int h = Integer.parseInt(heightInput.getText().toString());
                if (w > 0 && h > 0) {
                    getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit()
                            .putInt("res_width", w)
                            .putInt("res_height", h)
                            .apply();
                    android.widget.Toast.makeText(this, "Resolution saved", android.widget.Toast.LENGTH_SHORT).show();
                }
            } catch (Exception e) {}
        });

        resLayout.addView(widthInput);
        resLayout.addView(xText);
        resLayout.addView(heightInput);
        resLayout.addView(resResetButton);
        resLayout.addView(resApplyButton);

        android.widget.Button settingsBackButton = new android.widget.Button(this);
        settingsBackButton.setText("Back");
        
        settingsLayout.addView(loggingFiltersButton);
        settingsLayout.addView(loggingSpamFiltersButton);
        settingsLayout.addView(gpuOffloadButton);
        settingsLayout.addView(resLayout);
        settingsLayout.addView(onScreenDebugOverlayCheckbox);
        settingsLayout.addView(showPerfOverlayCheckbox);
        settingsLayout.addView(nativeRootMmapCheckbox);
        settingsLayout.addView(esModeButton);
        settingsLayout.addView(settingsBackButton, backParams);

        aboutButton.setOnClickListener(v -> {
            launcherGrid.setVisibility(View.GONE);
            bottomButtonsLayout.setVisibility(View.GONE);
            noGamesText.setVisibility(View.GONE);
            aboutLayout.setVisibility(View.VISIBLE);
        });

        settingsButton.setOnClickListener(v -> {
            launcherGrid.setVisibility(View.GONE);
            bottomButtonsLayout.setVisibility(View.GONE);
            noGamesText.setVisibility(View.GONE);
            settingsLayout.setVisibility(View.VISIBLE);
        });

        View.OnClickListener backAction = v -> {
            aboutLayout.setVisibility(View.GONE);
            settingsLayout.setVisibility(View.GONE);
            if (installedApps.isEmpty()) {
                noGamesText.setVisibility(View.VISIBLE);
            } else {
                launcherGrid.setVisibility(View.VISIBLE);
            }
            bottomButtonsLayout.setVisibility(View.VISIBLE);
        };
        aboutBackButton.setOnClickListener(backAction);
        settingsBackButton.setOnClickListener(backAction);

        rootLayout.addView(launcherGrid);
        rootLayout.addView(noGamesText);
        rootLayout.addView(unpackLayout);
        rootLayout.addView(deleteLayout);
        rootLayout.addView(scrollView);
        rootLayout.addView(aboutLayout);
        rootLayout.addView(commandsLayout);
        rootLayout.addView(settingsLayout);
        rootLayout.addView(loggingFiltersLayout);
        rootLayout.addView(loggingSpamFiltersLayout);
        rootLayout.addView(gpuOffloadLayout);
        rootLayout.addView(bottomButtonsLayout, bottomParams);

        TextView versionTextView = new TextView(this) {
            @Override
            protected void onDraw(android.graphics.Canvas canvas) {
                getPaint().setStyle(android.graphics.Paint.Style.STROKE);
                getPaint().setStrokeWidth(4f);
                setTextColor(Color.BLACK);
                super.onDraw(canvas);
                getPaint().setStyle(android.graphics.Paint.Style.FILL);
                setTextColor(Color.WHITE);
                super.onDraw(canvas);
            }
        };
        try {
            android.content.pm.PackageInfo pInfo = getPackageManager().getPackageInfo(getPackageName(), 0);
            versionTextView.setText("DamnWrapper32 (ARMv7) v" + pInfo.versionName);
        } catch (Exception e) {
            versionTextView.setText("DamnWrapper32 (ARMv7)");
        }
        versionTextView.setTextSize(14f);
        versionTextView.setTypeface(Typeface.DEFAULT_BOLD);
        FrameLayout.LayoutParams verParams = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
        verParams.gravity = Gravity.BOTTOM | Gravity.LEFT;
        verParams.setMargins(20, 20, 20, 20);
        rootLayout.addView(versionTextView, verParams);

        setContentView(rootLayout);

        try { new File(WORK_DIR + "damn32_log.txt").delete(); } catch(Exception e) {}

        String currentDateAndTime = new SimpleDateFormat("dd.MM.yyyy HH:mm:ss", Locale.getDefault()).format(new Date());
        addLog("=== Запуск DamnWrapper32 (ARMv7) ===");
        addLog("Дата и время: " + currentDateAndTime);

        final Thread.UncaughtExceptionHandler defaultHandler = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            String crashLog = "=== JAVA CRASH ===\n" + throwable.toString() + "\n";
            for (StackTraceElement element : throwable.getStackTrace()) {
                crashLog += "\tat " + element.toString() + "\n";
            }
            addLog(crashLog);
            if (defaultHandler != null) {
                defaultHandler.uncaughtException(thread, throwable);
            } else {
                System.exit(2);
            }
        });

        checkPermissions();
    }

    private void checkPermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {
                Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION);
                Uri uri = Uri.fromParts("package", getPackageName(), null);
                intent.setData(uri);
                startActivityForResult(intent, 228);
                return;
            }
        } else if (checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE, Manifest.permission.READ_EXTERNAL_STORAGE}, 1);
            return;
        }
        safeSetupDirectories();
    }

    private void safeSetupDirectories() {
        if (!isSetupStarted) {
            isSetupStarted = true;
            setupDirectories();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == 1) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                safeSetupDirectories();
            } else {
                android.widget.Toast.makeText(this, "Permission required for DamnWrapper32 (ARMv7)!", android.widget.Toast.LENGTH_LONG).show();
                finish();
            }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == 228) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                if (Environment.isExternalStorageManager()) {
                    safeSetupDirectories();
                } else {
                    android.widget.Toast.makeText(this, "Permission required for DamnWrapper32 (ARMv7)!", android.widget.Toast.LENGTH_LONG).show();
                    finish();
                }
            }
        }
    }

    private void setupDirectories() {
        try {
            boolean esm = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) && Environment.isExternalStorageManager();
            boolean mk = new File(WORK_DIR).mkdirs();
            File probe = new File(WORK_DIR + "__java_probe.txt");
            java.io.FileOutputStream p = new java.io.FileOutputStream(probe);
            p.write("ok".getBytes()); p.close();
            Log.e("DW32PROBE", "isExternalStorageManager=" + esm + " workMkdirs=" + mk +
                  " workExists=" + new File(WORK_DIR).exists() + " probeWritten=" + probe.exists() +
                  " canWrite=" + new File(WORK_DIR).canWrite());
        } catch (Exception e) {
            Log.e("DW32PROBE", "ПРОБНАЯ ЗАПИСЬ УПАЛА: " + e);
        }
        new File(WORK_DIR).mkdirs();
        new File(APPS_DIR).mkdirs();
        new File(SETUP_DIR).mkdirs();
        try {
            File optionsFile = new File(WORK_DIR + "damn32_options.txt");
            if (!optionsFile.exists()) {
                optionsFile.createNewFile();
            }
        } catch (Exception e) {}
        addLog("Root: Больше не требуется, используется механизм Total Rebase.");
        extractSetup();
        loadWallpaper();
        new Thread(this::scanAndUnpackApps).start();
    }

    private void loadWallpaper() {
        File setupDir = new File(SETUP_DIR);
        File bestWallpaper = null;
        long latestTime = 0;
        if (setupDir.exists()) {
            File[] files = setupDir.listFiles();
            if (files != null) {
                for (File f : files) {
                    if (f.isFile() && f.lastModified() > latestTime) {
                        BitmapFactory.Options opt = new BitmapFactory.Options();
                        opt.inJustDecodeBounds = true;
                        BitmapFactory.decodeFile(f.getAbsolutePath(), opt);
                        if (opt.outWidth > 0 && opt.outHeight > 0) {
                            bestWallpaper = f;
                            latestTime = f.lastModified();
                        }
                    }
                }
            }
        }

        View wallpaperView = null;
        if (bestWallpaper != null) {
            wallpaperView = createWallpaperView(this, bestWallpaper);
        }

        if (wallpaperView != null) {
            rootLayout.addView(wallpaperView, 0, new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        } else {
            TextView wallpaperHintText = new TextView(this);
            wallpaperHintText.setText("To use own wallpaper put image to DamnWrapper32_ARMv7/setup");
            wallpaperHintText.setTextColor(Color.GRAY);
            wallpaperHintText.setTextSize(10f);
            wallpaperHintText.setGravity(Gravity.CENTER);
            FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT);
            hintParams.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
            hintParams.bottomMargin = 280;
            // Устанавливаем index 0, чтобы текст был на заднем плане
            rootLayout.addView(wallpaperHintText, 0, hintParams);
        }
    }

    private void extractSetup() {
        try {
            String[] assets = getAssets().list("");
            for (String asset : assets) {
                try {
                    File outFile = new File(SETUP_DIR, asset);
                    if (!outFile.exists()) {
                        InputStream is = getAssets().open(asset);
                        FileOutputStream fos = new FileOutputStream(outFile);
                        byte[] buf = new byte[8192]; int len;
                        while ((len = is.read(buf)) > 0) fos.write(buf, 0, len);
                        is.close(); fos.close();
                    }
                } catch (Exception innerE) {}
            }
        } catch (Exception e) {}
    }

    // Каталог приложения на /sdcard/Android/data примонтирован f2fs в обход FUSE,
    // поэтому mkdir там работает, а на /sdcard MediaProvider отдаёт EEXIST для удалённых путей.
    private String sandboxRootFor(String bundleId) {
        File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        return new File(base, "sandbox/" + bundleId).getAbsolutePath() + "/";
    }

    private void startGameThread(String workDir, String appDirPath, String bundleId, int logMask, int spamMask, boolean onScreenDebugOverlay, boolean showPerfOverlay, boolean nativeRootMmap, int targetW, int targetH, int esMode, int gpuOffloadMask) {
        new Thread(() -> {
            if (nativeRootMmap) {
                try {
                    Runtime.getRuntime().exec(new String[]{"su", "-c", "setenforce 1"}).waitFor();
                    Thread.sleep(500);
                    Runtime.getRuntime().exec(new String[]{"su", "-c", "setenforce 0"}).waitFor();
                    Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", "getenforce"});
                    java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()));
                    String selinuxStatus = reader.readLine();
                    if (selinuxStatus == null || (!selinuxStatus.toLowerCase().contains("permissive") && !selinuxStatus.toLowerCase().contains("disabled"))) {
                        runOnUiThread(() -> addLog("ОШИБКА: Не удалось перевести SELinux в Permissive!"));
                        return;
                    }
                } catch (Exception e) {
                    runOnUiThread(() -> addLog("ОШИБКА: Нет Root прав или сбой SELinux!"));
                    return;
                }
            }
            String sb = sandboxRootFor(bundleId);
            try {
                new File(sb + "Documents").mkdirs();
                new File(sb + "tmp").mkdirs();
                new File(sb + "Library/Caches").mkdirs();
                new File(sb + "Library/Preferences").mkdirs();
                Log.e("DW32PROBE", "sandbox базовый создан: " + new File(sb + "Documents").exists() + " -> " + sb);
            } catch (Exception e) {
                Log.e("DW32PROBE", "sandbox mkdirs упал: " + e);
            }
            initWrapper(workDir, sb, appDirPath, bundleId, logMask, spamMask, onScreenDebugOverlay, showPerfOverlay, nativeRootMmap, targetW, targetH, esMode, gpuOffloadMask);
        }).start();
    }

    private static int countSlashes(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) == '/') n++;
        return n;
    }

    private void copyZipEntry(java.util.zip.ZipFile zip, java.util.zip.ZipEntry entry, File dst) throws Exception {
        dst.getParentFile().mkdirs();
        try (InputStream is = zip.getInputStream(entry); FileOutputStream fos = new FileOutputStream(dst)) {
            byte[] buffer = new byte[65536]; int len;
            while ((len = is.read(buffer)) > 0) fos.write(buffer, 0, len);
        }
    }

    // Имя иконки по Info.plist, иначе типовые варианты.
    private java.util.List<String> iconCandidates(HashMap<String, Object> plist) {
        java.util.List<String> names = new ArrayList<>();
        String iconName = (String) plist.get("CFBundleIconFile");
        if (iconName == null && plist.get("CFBundleIcons") instanceof HashMap) {
            HashMap icons = (HashMap) plist.get("CFBundleIcons");
            if (icons.get("CFBundlePrimaryIcon") instanceof HashMap) {
                HashMap primary = (HashMap) icons.get("CFBundlePrimaryIcon");
                if (primary.get("CFBundleIconFiles") instanceof ArrayList) {
                    ArrayList list = (ArrayList) primary.get("CFBundleIconFiles");
                    if (!list.isEmpty()) iconName = (String) list.get(list.size() - 1);
                }
            }
        }
        if (iconName != null) names.add(iconName.endsWith(".png") ? iconName : iconName + ".png");
        names.add("Icon@2x.png"); names.add("Icon-72.png"); names.add("Icon-72@2x.png");
        names.add("Icon.png"); names.add("icon.png");
        return names;
    }

    private void fillCommonInfo(AppInfo info, HashMap<String, Object> plist, String fallbackName, String prefsKey) {
        info.bundleId = (String) plist.getOrDefault("CFBundleIdentifier", "unknown");
        info.version = (String) plist.getOrDefault("CFBundleVersion", "1.0");
        info.name = (String) plist.getOrDefault("CFBundleDisplayName", plist.getOrDefault("CFBundleName", fallbackName));
        String customName = getSharedPreferences("DamnPrefs", MODE_PRIVATE).getString("custom_name_" + prefsKey, null);
        if (customName != null) info.name = customName;
        info.minOS = (String) plist.getOrDefault("MinimumOSVersion", "Unknown");
        info.targetOS = (String) plist.getOrDefault("DTPlatformVersion", "Unknown");
        Object familyObj = plist.get("UIDeviceFamily");
        if (familyObj instanceof ArrayList) {
            ArrayList famList = (ArrayList) familyObj;
            boolean hasPhone = famList.contains(1) || famList.contains(1L) || famList.contains("1");
            boolean hasPad = famList.contains(2) || famList.contains(2L) || famList.contains("2");
            if (hasPhone && hasPad) info.deviceFamily = "Universal";
            else if (hasPad) info.deviceFamily = "iPad";
            else if (hasPhone) info.deviceFamily = "iPhone";
            else info.deviceFamily = "Unknown";
        } else if (familyObj != null) {
            String f = familyObj.toString();
            if (f.equals("1")) info.deviceFamily = "iPhone";
            else if (f.equals("2")) info.deviceFamily = "iPad";
            else info.deviceFamily = f;
        } else {
            info.deviceFamily = "iPhone";
        }
    }

    // Игра берётся прямо из .ipa: распаковки нет, наружу вытаскивается только иконка.
    private AppInfo readIpaApp(File ipa) {
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(ipa)) {
            java.util.zip.ZipEntry plistEntry = null;
            String appPrefix = null;
            java.util.Enumeration<? extends java.util.zip.ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                java.util.zip.ZipEntry entry = entries.nextElement();
                String n = entry.getName();
                if (n.startsWith("Payload/") && n.endsWith(".app/Info.plist") && countSlashes(n) == 2) {
                    plistEntry = entry;
                    appPrefix = n.substring(0, n.length() - "Info.plist".length());
                    break;
                }
            }
            if (plistEntry == null) return null;

            File tempPlist = new File(getCacheDir(), "temp_Info.plist");
            copyZipEntry(zip, plistEntry, tempPlist);
            HashMap<String, Object> plist = BplistParser.parse(tempPlist);
            tempPlist.delete();

            AppInfo info = new AppInfo();
            info.appDirPath = ipa.getAbsolutePath();
            info.internalName = appPrefix.substring("Payload/".length(), appPrefix.length() - 1);
            fillCommonInfo(info, plist, ipa.getName(), ipa.getName());
            info.iconPath = extractIpaIcon(zip, ipa, appPrefix, plist);
            return info;
        } catch (Exception e) {
            return null;
        }
    }

    private String extractIpaIcon(java.util.zip.ZipFile zip, File ipa, String appPrefix, HashMap<String, Object> plist) {
        File cacheDir = new File(getCacheDir(), "icons");
        cacheDir.mkdirs();
        File out = new File(cacheDir, ipa.getName() + "_" + ipa.lastModified() + ".png");
        if (out.exists()) return out.getAbsolutePath();

        java.util.List<String> candidates = new ArrayList<>();
        candidates.add("iTunesArtwork");
        candidates.add(appPrefix + "iTunesArtwork");
        for (String n : iconCandidates(plist)) candidates.add(appPrefix + n);

        for (String name : candidates) {
            java.util.zip.ZipEntry e = zip.getEntry(name);
            if (e == null) continue;
            try {
                copyZipEntry(zip, e, out);
                return out.getAbsolutePath();
            } catch (Exception ex) {}
        }
        return null;
    }

    // Рядом с .ipa допустима и распакованная папка с Payload/<Имя>.app — например, мод.
    private AppInfo readUnpackedApp(File gameFolder) {
        File payload = new File(gameFolder, "Payload");
        File[] children = payload.listFiles();
        if (children == null) return null;
        for (File appDir : children) {
            if (!appDir.getName().endsWith(".app")) continue;
            HashMap<String, Object> plist = BplistParser.parse(new File(appDir, "Info.plist"));
            AppInfo info = new AppInfo();
            info.appDirPath = appDir.getAbsolutePath();
            info.internalName = appDir.getName();
            fillCommonInfo(info, plist, gameFolder.getName(), gameFolder.getName());

            File artworkRoot = new File(gameFolder, "iTunesArtwork");
            File artworkApp = new File(appDir, "iTunesArtwork");
            if (artworkRoot.exists()) info.iconPath = artworkRoot.getAbsolutePath();
            else if (artworkApp.exists()) info.iconPath = artworkApp.getAbsolutePath();
            else {
                for (String n : iconCandidates(plist)) {
                    File f = new File(appDir, n);
                    if (f.exists()) { info.iconPath = f.getAbsolutePath(); break; }
                }
            }
            return info;
        }
        return null;
    }

    private void scanAndUnpackApps() {
        try {
            File optionsFile = new File(WORK_DIR + "damn32_options.txt");
            if (optionsFile.exists()) {
                java.io.BufferedReader reader = new java.io.BufferedReader(new java.io.FileReader(optionsFile));
                String line;
                boolean hasError = false;
                String targetApp = null;
                boolean noVideo = false;
                while ((line = reader.readLine()) != null) {
                    line = line.trim();
                    if (line.isEmpty()) continue;
                    
                    int firstSpace = line.indexOf(' ');
                    if (firstSpace != -1) {
                        String rest = line.substring(firstSpace + 1).trim();
                        if (rest.contains(" -") || rest.startsWith("-")) {
                            hasError = true;
                            break;
                        }
                    } else if (line.contains(" -")) {
                        hasError = true;
                        break;
                    }
                    
                    if (line.startsWith("-launch ")) {
                        targetApp = line.substring(8).trim();
                    } else if (line.startsWith("-novideo")) {
                        noVideo = true;
                    }
                }
                reader.close();

                if (hasError) {
                    final String failMsg = "Command error: multiple commands on one line are not allowed";
                    runOnUiThread(() -> android.widget.Toast.makeText(MainActivity.this, failMsg, android.widget.Toast.LENGTH_LONG).show());
                } else {
                    getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().putBoolean("novideo_cmd", noVideo).apply();
                    if (targetApp != null) {
                        File target = new File(APPS_DIR, targetApp);
                        AppInfo autoInfo = null;
                        if (target.isFile() && targetApp.toLowerCase(Locale.US).endsWith(".ipa")) autoInfo = readIpaApp(target);
                        else if (target.isDirectory()) autoInfo = readUnpackedApp(target);
                        if (autoInfo != null) {
                            final AppInfo finalInfo = autoInfo;
                            runOnUiThread(() -> launchApp(finalInfo, true));
                            return;
                        }
                        final String failMsg = "Command error: no app " + targetApp + " founded";
                        runOnUiThread(() -> android.widget.Toast.makeText(MainActivity.this, failMsg, android.widget.Toast.LENGTH_LONG).show());
                    }
                }
            }
        } catch (Exception e) {}

        // Игры лежат прямо в apps: либо .ipa (читается виртуальным диском), либо распакованная папка.
        installedApps.clear();
        File appsDir = new File(APPS_DIR);
        File[] entries = appsDir.listFiles();
        if (entries != null) {
            java.util.Arrays.sort(entries, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            java.util.HashSet<String> present = new java.util.HashSet<>();
            for (File f : entries) {
                AppInfo info = null;
                if (f.isFile() && f.getName().toLowerCase(Locale.US).endsWith(".ipa")) info = readIpaApp(f);
                else if (f.isDirectory()) info = readUnpackedApp(f);
                if (info == null) continue;
                info.sourcePath = f.getAbsolutePath();
                present.add(f.getName());
                installedApps.add(info);
            }
            android.content.SharedPreferences prefs = getSharedPreferences("DamnPrefs", MODE_PRIVATE);
            for (java.util.Map.Entry<String, ?> e : prefs.getAll().entrySet()) {
                if (e.getKey().startsWith("custom_name_") && !present.contains(e.getKey().substring(12))) {
                    prefs.edit().remove(e.getKey()).apply();
                }
            }
        }

        runOnUiThread(() -> {
            unpackLayout.setVisibility(View.GONE);
            if (installedApps.isEmpty()) {
                noGamesText.setVisibility(View.VISIBLE);
            } else {
                launcherGrid.setVisibility(View.VISIBLE);
                launcherGrid.setAdapter(new AppsAdapter());
            }
            if (bottomButtonsLayout != null) bottomButtonsLayout.setVisibility(View.VISIBLE);
        });
    }

    // Сжатый .ipa читать дорого: inflate на каждое чтение. Поэтому при запуске архив
    // один раз перепаковывается в STORED — дальше игра читает данные как с обычного диска.
    private boolean ipaNeedsRepack(File ipa) {
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(ipa)) {
            java.util.Enumeration<? extends java.util.zip.ZipEntry> en = zip.entries();
            while (en.hasMoreElements()) {
                if (en.nextElement().getMethod() != java.util.zip.ZipEntry.STORED) return true;
            }
        } catch (Exception e) {}
        return false;
    }

    private void repackStored(File ipa) {
        File tmp = new File(ipa.getParentFile(), ipa.getName() + ".opt");
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(ipa);
             java.util.zip.ZipOutputStream out = new java.util.zip.ZipOutputStream(
                 new java.io.BufferedOutputStream(new FileOutputStream(tmp), 1 << 20))) {
            out.setMethod(java.util.zip.ZipOutputStream.STORED);
            int total = Math.max(1, zip.size());
            int done = 0, lastPct = -1;
            byte[] buf = new byte[1 << 16];
            java.util.Enumeration<? extends java.util.zip.ZipEntry> en = zip.entries();
            while (en.hasMoreElements()) {
                java.util.zip.ZipEntry e = en.nextElement();
                java.util.zip.ZipEntry ne = new java.util.zip.ZipEntry(e.getName());
                ne.setMethod(java.util.zip.ZipEntry.STORED);
                ne.setTime(e.getTime());
                long size = e.isDirectory() ? 0 : e.getSize();
                long crc = e.getCrc();
                if (size < 0 || crc < 0) throw new java.io.IOException("нет размера/CRC у " + e.getName());
                ne.setSize(size);
                ne.setCompressedSize(size);
                ne.setCrc(crc);
                out.putNextEntry(ne);
                if (!e.isDirectory()) {
                    try (InputStream is = zip.getInputStream(e)) {
                        int r;
                        while ((r = is.read(buf)) > 0) out.write(buf, 0, r);
                    }
                }
                out.closeEntry();
                done++;
                int pct = done * 100 / total;
                if (pct != lastPct && pct % 10 == 0) {
                    lastPct = pct;
                    final int p = pct;
                    runOnUiThread(() -> addLog("IPA: перепаковка без сжатия " + p + "%"));
                }
            }
        } catch (Exception ex) {
            tmp.delete();
            runOnUiThread(() -> addLog("IPA: перепаковка не удалась (" + ex + "), играем как есть"));
            return;
        }
        File bak = new File(ipa.getAbsolutePath() + ".bak");
        if (ipa.renameTo(bak)) {
            if (tmp.renameTo(ipa)) bak.delete();
            else { tmp.delete(); bak.renameTo(ipa); }
        } else {
            tmp.delete();
        }
    }

    private void launchApp(AppInfo app, boolean autoLaunch) {
        launcherGrid.setVisibility(View.GONE);
        if (bottomButtonsLayout != null) bottomButtonsLayout.setVisibility(View.GONE);
        scrollView.setVisibility(View.VISIBLE);
        logTextView.setText("");
        try { new File(WORK_DIR + "damn32_log.txt").delete(); } catch (Exception e) {}

        android.content.SharedPreferences prefs = getSharedPreferences("DamnPrefs", MODE_PRIVATE);
        addLog(autoLaunch ? "=== Запуск DamnWrapper32 (ARMv7) (Auto-launch) ===" : "=== Запуск DamnWrapper32 (ARMv7) ===");
        addLog("Дата и время: " + new SimpleDateFormat("dd.MM.yyyy HH:mm:ss", Locale.getDefault()).format(new Date()));
        boolean nativeRootMmap = prefs.getBoolean("native_root_mmap", false);
        if (nativeRootMmap) addLog("ВНИМАНИЕ: Включен Native root mmap! Игра будет загружена по оригинальным адресам.");
        else addLog("Механизм Total Rebase активен, игра будет загружена по динамическому смещению.");
        if (!new File(SETUP_DIR, "Roboto-VariableFont_wdth,wght.ttf").exists()) {
            addLog("HLE: ОШИБКА загрузки TTF шрифта! Проверьте наличие в папке setup.");
        }
        addLog("Picked: " + app.appDirPath);
        addLog("- Name: " + app.name);
        addLog("- Version: " + app.version);
        addLog("- Identifier: " + app.bundleId);
        addLog("- Internal name (canonical): " + app.internalName);
        addLog("- Minimum IOS version: " + app.minOS);
        addLog("- Target IOS version: " + app.targetOS);
        addLog("- Device family: " + app.deviceFamily);

        final int finalLogMask = buildLogMask();
        boolean onScreenDebugOverlay = prefs.getBoolean("onscreen_debug_overlay", false);
        boolean showPerfOverlay = prefs.getBoolean("show_perf_overlay", false);
        int targetW = prefs.getInt("res_width", 480);
        int targetH = prefs.getInt("res_height", 320);
        int esMode = prefs.getInt("es_mode", 2);
        final int finalSpamMask = buildSpamMask();
        int gpuMask = 0;
        for (int j = 0; j < 7; j++) if (prefs.getBoolean("gpu_bit_" + j, true)) gpuMask |= (1 << j);
        final int finalGpuMask = gpuMask;

        new Thread(() -> {
            File src = new File(app.appDirPath);
            if (app.appDirPath.toLowerCase(Locale.US).endsWith(".ipa") && ipaNeedsRepack(src)) {
                runOnUiThread(() -> addLog("IPA: сжатый архив, перепаковываю без сжатия — это разово"));
                repackStored(src);
            }
            startGameThread(WORK_DIR, app.appDirPath, app.bundleId, finalLogMask, finalSpamMask, onScreenDebugOverlay, showPerfOverlay, nativeRootMmap, targetW, targetH, esMode, finalGpuMask);
        }).start();
    }

    // Класс-модель игры
    private static class AppInfo { String name, bundleId, version, iconPath, appDirPath, sourcePath, internalName, minOS, targetOS, deviceFamily; }

    // Адаптер сетки лаунчера
    private class AppsAdapter extends BaseAdapter {
        @Override public int getCount() { return installedApps.size(); }
        @Override public Object getItem(int i) { return installedApps.get(i); }
        @Override public long getItemId(int i) { return i; }
        @Override public View getView(int i, View view, ViewGroup viewGroup) {
            LinearLayout layout = new LinearLayout(MainActivity.this);
            layout.setOrientation(LinearLayout.VERTICAL);
            layout.setGravity(Gravity.CENTER);
            
            AppInfo app = installedApps.get(i);
            ImageView icon = new ImageView(MainActivity.this);
            icon.setLayoutParams(new LinearLayout.LayoutParams(160, 160));
            icon.setClipToOutline(true);
            
            GradientDrawable fallbackBg = new GradientDrawable();
            fallbackBg.setColor(Color.DKGRAY);
            fallbackBg.setCornerRadius(30f);
            
            if (app.iconPath != null && new File(app.iconPath).exists()) {
                Bitmap bmp = BitmapFactory.decodeFile(app.iconPath);
                // Проверка на CgBI-проблему: если Android загрузил битмап, но он полностью прозрачный (сбой альфа-канала)
                if (bmp != null) {
                    boolean isSuspicious = false;
                    if (bmp.getWidth() > 0 && bmp.getHeight() > 0) {
                        int centerPixel = bmp.getPixel(bmp.getWidth() / 2, bmp.getHeight() / 2);
                        if (Color.alpha(centerPixel) == 0) { // Центр иконки IOS никогда не должен быть абсолютно прозрачным
                            isSuspicious = true;
                        }
                    }
                    if (!isSuspicious) {
                        icon.setImageBitmap(bmp);
                    } else {
                        setFallbackIcon(icon, fallbackBg);
                    }
                } else {
                    setFallbackIcon(icon, fallbackBg);
                }
            } else {
                setFallbackIcon(icon, fallbackBg);
            }

            FrameLayout iconContainer = new FrameLayout(MainActivity.this);
            iconContainer.setLayoutParams(new LinearLayout.LayoutParams(160, 160));
            icon.setLayoutParams(new FrameLayout.LayoutParams(160, 160));
            iconContainer.addView(icon);

            TextView versionText = new TextView(MainActivity.this) {
                @Override
                protected void onDraw(android.graphics.Canvas canvas) {
                    getPaint().setStyle(android.graphics.Paint.Style.STROKE);
                    getPaint().setStrokeWidth(4f);
                    setTextColor(Color.BLACK);
                    super.onDraw(canvas);
                    getPaint().setStyle(android.graphics.Paint.Style.FILL);
                    setTextColor(Color.WHITE);
                    super.onDraw(canvas);
                }
            };
            versionText.setText(app.version);
            versionText.setTextSize(10f);
            versionText.setSingleLine(true);
            versionText.setEllipsize(android.text.TextUtils.TruncateAt.END);
            versionText.setGravity(Gravity.CENTER);
            versionText.setTypeface(Typeface.DEFAULT_BOLD);
            versionText.setPadding(4, 0, 4, 0);
            FrameLayout.LayoutParams verParams = new FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.WRAP_CONTENT);
            verParams.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
            verParams.bottomMargin = 2;
            iconContainer.addView(versionText, verParams);

            TextView name = new TextView(MainActivity.this);
            name.setText(app.name);
            name.setTextColor(Color.WHITE);
            name.setTextSize(12f);
            name.setGravity(Gravity.CENTER);
            name.setPadding(0, 10, 0, 0);
            name.setSingleLine(true);
            
            layout.addView(iconContainer); layout.addView(name);
            
            layout.setOnLongClickListener(v -> {
                String prefsKey = new File(app.sourcePath).getName();
                new android.app.AlertDialog.Builder(MainActivity.this)
                    .setTitle(app.name)
                    .setItems(new String[]{"Rename app", "Delete app"}, (dialog, which) -> {
                        if (which == 0) {
                            android.widget.EditText input = new android.widget.EditText(MainActivity.this);
                            input.setText(app.name);
                            new android.app.AlertDialog.Builder(MainActivity.this)
                                .setTitle("Rename app")
                                .setView(input)
                                .setPositiveButton("OK", (d, w) -> {
                                    String newName = input.getText().toString();
                                    getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit()
                                        .putString("custom_name_" + prefsKey, newName).apply();
                                    app.name = newName;
                                    notifyDataSetChanged();
                                })
                                .setNegativeButton("Cancel", null)
                                .show();
                        } else {
                            deleteAppWithProgress(app);
                        }
                    })
                    .show();
                return true;
            });

            layout.setOnClickListener(v -> launchApp(app, false));
            return layout;
        }

        private void setFallbackIcon(ImageView iv, GradientDrawable bg) {
            iv.setBackground(bg);
            iv.setImageTintList(android.content.res.ColorStateList.valueOf(Color.WHITE));
            // Рисуем восклицательный знак
            Bitmap bmp = Bitmap.createBitmap(160, 160, Bitmap.Config.ARGB_8888);
            android.graphics.Canvas c = new android.graphics.Canvas(bmp);
            android.graphics.Paint p = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
            p.setColor(Color.WHITE); p.setTextSize(80f); p.setTypeface(Typeface.DEFAULT_BOLD); p.setTextAlign(android.graphics.Paint.Align.CENTER);
            c.drawText("!", 80, 110, p);
            iv.setImageBitmap(bmp);
        }
    }

    // Суперкомпактный парсер Apple Binary PList для Info.plist (Поддерживает Dict, Array, Strings)
    public static class BplistParser {
        public static HashMap<String, Object> parse(File file) {
            HashMap<String, Object> result = new HashMap<>();
            try {
                byte[] data = new byte[(int) file.length()];
                FileInputStream fis = new FileInputStream(file); fis.read(data); fis.close();
                if (data.length < 40 || !new String(data, 0, 8).equals("bplist00")) return extractXmlFallback(new String(data));
                
                int trailerOff = data.length - 32;
                int offsetSize = data[trailerOff + 6];
                int refSize = data[trailerOff + 7];
                int numObjects = (int) readInt(data, trailerOff + 12, 4);
                int topObj = (int) readInt(data, trailerOff + 20, 4);
                int offsetTableOff = (int) readInt(data, trailerOff + 28, 4);
                
                int[] offsets = new int[numObjects];
                for (int i = 0; i < numObjects; i++) offsets[i] = (int) readInt(data, offsetTableOff + i * offsetSize, offsetSize);
                
                Object top = decodeObj(data, offsets, topObj, refSize);
                if (top instanceof HashMap) return (HashMap<String, Object>) top;
            } catch (Exception e) {}
            return result;
        }

        private static HashMap<String, Object> extractXmlFallback(String xml) {
            HashMap<String, Object> res = new HashMap<>();
            String[] keys = {"CFBundleIdentifier", "CFBundleVersion", "CFBundleDisplayName", "CFBundleName", "CFBundleIconFile", "MinimumOSVersion", "DTPlatformVersion"};
            int uidIdx = xml.indexOf("<key>UIDeviceFamily</key>");
            if (uidIdx != -1) {
                int arrStart = xml.indexOf("<array>", uidIdx);
                int arrEnd = xml.indexOf("</array>", arrStart);
                if (arrStart > 0 && arrEnd > arrStart) {
                    String arrXml = xml.substring(arrStart, arrEnd);
                    if (arrXml.contains("1") && arrXml.contains("2")) res.put("UIDeviceFamily", "Universal");
                    else if (arrXml.contains("2")) res.put("UIDeviceFamily", "iPad");
                    else res.put("UIDeviceFamily", "iPhone");
                }
            }
            for (String k : keys) {
                int idx = xml.indexOf("<key>" + k + "</key>");
                if (idx != -1) {
                    int start = xml.indexOf("<string>", idx) + 8;
                    int end = xml.indexOf("</string>", start);
                    if (start > 7 && end > start) res.put(k, xml.substring(start, end));
                }
            }
            return res;
        }

        private static Object decodeObj(byte[] data, int[] offsets, int objIdx, int refSize) {
            int off = offsets[objIdx];
            int type = data[off] & 0xFF;
            int objType = type & 0xF0;
            if (objType == 0x10) {
                int len = (int) Math.pow(2, type & 0x0F);
                return readInt(data, off + 1, len);
            } else if (objType == 0x50 || objType == 0x60) {
                int len = type & 0x0F; int start = off + 1;
                if (len == 0x0F) {
                    int intType = data[++off] & 0xFF; int intLen = (int) Math.pow(2, intType & 0x0F);
                    len = (int) readInt(data, ++off, intLen); start = off + intLen;
                }
                try { return objType == 0x50 ? new String(data, start, len, "ASCII") : new String(data, start, len * 2, "UTF-16BE"); } catch(Exception e) { return ""; }
            } else if (objType == 0xD0) {
                int count = type & 0x0F; int start = off + 1;
                if (count == 0x0F) {
                    int intType = data[++off] & 0xFF; int intLen = (int) Math.pow(2, intType & 0x0F);
                    count = (int) readInt(data, ++off, intLen); start = off + intLen;
                }
                HashMap<String, Object> dict = new HashMap<>();
                for (int i = 0; i < count; i++) {
                    int keyRef = (int) readInt(data, start + i * refSize, refSize);
                    int valRef = (int) readInt(data, start + (count + i) * refSize, refSize);
                    Object key = decodeObj(data, offsets, keyRef, refSize);
                    Object val = decodeObj(data, offsets, valRef, refSize);
                    if (key != null && val != null) dict.put(key.toString(), val);
                }
                return dict;
            } else if (objType == 0xA0) {
                int count = type & 0x0F; int start = off + 1;
                if (count == 0x0F) {
                    int intType = data[++off] & 0xFF; int intLen = (int) Math.pow(2, intType & 0x0F);
                    count = (int) readInt(data, ++off, intLen); start = off + intLen;
                }
                ArrayList<Object> list = new ArrayList<>();
                for (int i = 0; i < count; i++) {
                    int valRef = (int) readInt(data, start + i * refSize, refSize);
                    list.add(decodeObj(data, offsets, valRef, refSize));
                }
                return list;
            }
            return null;
        }

        private static long readInt(byte[] data, int off, int len) {
            long res = 0; for (int i = 0; i < len; i++) res = (res << 8) | (data[off + i] & 0xFF); return res;
        }
    }

    public void showArchErrorPopup(String arch) {
        runOnUiThread(() -> {
            new android.app.AlertDialog.Builder(MainActivity.this)
                .setTitle("App not supported")
                .setMessage("This app is ARMv" + arch + " - it's not supported, use only ARMv7 apps")
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> {
                    scrollView.setVisibility(View.GONE);
                    if (installedApps.isEmpty()) {
                        noGamesText.setVisibility(View.VISIBLE);
                    } else {
                        launcherGrid.setVisibility(View.VISIBLE);
                    }
                    if (bottomButtonsLayout != null) bottomButtonsLayout.setVisibility(View.VISIBLE);
                })
                .show();
        });
    }

    public void showDrmErrorPopup() {
        runOnUiThread(() -> {
            new android.app.AlertDialog.Builder(MainActivity.this)
                .setTitle("App not supported")
                .setMessage("IPA is encrypted, bad file")
                .setCancelable(false)
                .setPositiveButton("OK", (d, w) -> {
                    scrollView.setVisibility(View.GONE);
                    if (installedApps.isEmpty()) {
                        noGamesText.setVisibility(View.VISIBLE);
                    } else {
                        launcherGrid.setVisibility(View.VISIBLE);
                    }
                    if (bottomButtonsLayout != null) bottomButtonsLayout.setVisibility(View.VISIBLE);
                })
                .show();
        });
    }

    private void deleteAppWithProgress(AppInfo app) {
        File target = new File(app.sourcePath);
        if (!target.exists()) return;

        deleteLayout.setVisibility(View.VISIBLE);
        launcherGrid.setVisibility(View.GONE);
        if (bottomButtonsLayout != null) bottomButtonsLayout.setVisibility(View.GONE);
        deleteText.setText("Deleting...");
        deleteAppName.setText("App: " + app.name);
        deletePkgName.setText("Package: " + app.bundleId);
        deleteVersion.setText("Version: " + app.version);
        deleteProgress.setProgress(0);

        new Thread(() -> {
            if (target.isDirectory()) {
                // counts[0] = total, counts[1] = deleted, counts[2] = lastReportedProgress
                int[] counts = new int[]{0, 0, 0};
                countFiles(target, counts);
                deleteFilesProgress(target, counts);
            }
            target.delete();
            getSharedPreferences("DamnPrefs", MODE_PRIVATE).edit().remove("custom_name_" + target.getName()).apply();

            runOnUiThread(() -> {
                deleteLayout.setVisibility(View.GONE);
                new Thread(this::scanAndUnpackApps).start();
            });
        }).start();
    }
    
    private void countFiles(File dir, int[] counts) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            counts[0]++;
            if (f.isDirectory()) countFiles(f, counts);
        }
    }

    private void deleteFilesProgress(File dir, int[] counts) {
        File[] files = dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            if (f.isDirectory()) deleteFilesProgress(f, counts);
            f.delete();
            counts[1]++;
            int progress = (int) ((counts[1] * 100f) / counts[0]);
            if (progress > counts[2]) {
                counts[2] = progress;
                runOnUiThread(() -> deleteProgress.setProgress(progress));
            }
        }
    }

    private final StringBuilder pendingLog = new StringBuilder();
    private boolean logFlushScheduled = false;
    private final Runnable logFlushTask = new Runnable() {
        @Override public void run() {
            String chunk;
            synchronized (pendingLog) {
                logFlushScheduled = false;
                if (pendingLog.length() == 0) return;
                chunk = pendingLog.toString();
                pendingLog.setLength(0);
            }
            logTextView.append(chunk);
            CharSequence all = logTextView.getText();
            if (all.length() > 200000) {
                logTextView.setText(all.subSequence(all.length() - 150000, all.length()));
            }
            scrollView.fullScroll(View.FOCUS_DOWN);
        }
    };

    // Пачкой раз в 250 мс: построчный append в TextView через runOnUiThread затыкал поток игры
    // насмерть (18 тыс. строк за прогон = 18 тыс. постов в UI-очередь).
    public void addLogFromNative(String msg) {
        if (isRendering) return; // лог-вью не на экране, вся история и так лежит в damn32_log.txt
        synchronized (pendingLog) {
            if (pendingLog.length() > 400000) return;
            pendingLog.append(msg).append('\n');
            if (logFlushScheduled) return;
            logFlushScheduled = true;
        }
        logTextView.postDelayed(logFlushTask, 250);
    }

    @SuppressLint("ClickableViewAccessibility")
    public void switchToRender() {
        runOnUiThread(() -> {
            if (isRendering) return;
            isRendering = true;
            addLog("Окно подготовлено. Ожидание вызовов отрисовки от игры...");
            setLogUiVisible(false);

            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);

            surfaceView = new SurfaceView(this);
            surfaceView.getHolder().addCallback(this);

            FrameLayout renderContainer = new FrameLayout(this);
            renderContainer.setBackgroundColor(Color.BLACK);

            DisplayMetrics metrics = new DisplayMetrics();
            getWindowManager().getDefaultDisplay().getRealMetrics(metrics);
            
            int screenW = Math.max(metrics.widthPixels, metrics.heightPixels);
            int screenH = Math.min(metrics.widthPixels, metrics.heightPixels);

            int targetW = getSharedPreferences("DamnPrefs", MODE_PRIVATE).getInt("res_width", 480);
            int targetH = getSharedPreferences("DamnPrefs", MODE_PRIVATE).getInt("res_height", 320);

            float scaleX_scr = (float) screenW / targetW;
            float scaleY_scr = (float) screenH / targetH;
            float scale = Math.min(scaleX_scr, scaleY_scr);

            int scaledWidth = (int) (targetW * scale);
            int scaledHeight = (int) (targetH * scale);

            scaleFactorX = (float) scaledWidth / targetW;
            scaleFactorY = (float) scaledHeight / targetH;

            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(scaledWidth, scaledHeight);
            params.gravity = Gravity.CENTER;
            surfaceView.setLayoutParams(params);

            surfaceView.setOnTouchListener((v, event) -> {
                int actionMasked = event.getActionMasked();
                float scaleX = (float) targetW / v.getWidth();
                float scaleY = (float) targetH / v.getHeight();

                if (actionMasked == MotionEvent.ACTION_MOVE) {
                    for (int i = 0; i < event.getPointerCount(); i++) {
                        int pId = event.getPointerId(i);
                        onTouchEventNative(actionMasked, pId, event.getX(i) * scaleX, event.getY(i) * scaleY);
                    }
                } else {
                    int pIndex = event.getActionIndex(); int pId = event.getPointerId(pIndex);
                    onTouchEventNative(actionMasked, pId, event.getX(pIndex) * scaleX, event.getY(pIndex) * scaleY);
                }
                return true;
            });

            renderContainer.addView(surfaceView);

            // --- ИНИЦИАЛИЗАЦИЯ ВИДЕО UI ---
            videoContainer = new FrameLayout(this);
            videoContainer.setBackgroundColor(Color.BLACK);
            videoContainer.setVisibility(View.GONE);

            videoView = new android.widget.VideoView(this);
            videoView.setZOrderMediaOverlay(true); // ФИКС Z-ORDER: Заставляет Surface видеоплеера рендериться над OpenGL и правильно прятаться
            videoView.setVisibility(View.GONE); // ФИКС: Явное скрытие Surface

            FrameLayout.LayoutParams vvParams = new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT);
            vvParams.gravity = Gravity.CENTER;
            videoContainer.addView(videoView, vvParams);

            skipButton = new android.widget.Button(this);
            skipButton.setText("Skip");
            skipButton.setVisibility(View.GONE);
            FrameLayout.LayoutParams btnParams = new FrameLayout.LayoutParams(300, 150);
            btnParams.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
            btnParams.bottomMargin = 80;
            videoContainer.addView(skipButton, btnParams);

            videoContainer.setOnTouchListener((v, ev) -> {
                if (ev.getAction() == MotionEvent.ACTION_DOWN) {
                    skipButton.setVisibility(View.VISIBLE);
                    if (hideSkipRunnable != null) videoHandler.removeCallbacks(hideSkipRunnable);
                    hideSkipRunnable = () -> skipButton.setVisibility(View.GONE);
                    videoHandler.postDelayed(hideSkipRunnable, 3000);
                }
                return true;
            });

            skipButton.setOnClickListener(v -> stopVideo());
            videoView.setOnCompletionListener(mp -> stopVideo());

            renderContainer.addView(videoContainer, new FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

            setContentView(renderContainer);
            
            getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            );
        });
    }

    // ==========================================
    // AUDIO IMPLEMENTATION NATIVE CALLS
    // ==========================================
    public void audioInit(int ptrId, String path) {
        try {
            MediaPlayer player = new MediaPlayer();
            player.setDataSource(path);
            player.prepare();
            audioPlayers.put(ptrId, player);
            addLogFromNative("HLE Audio: Успешно загружен трек " + path);
        } catch (Exception e) {
            addLogFromNative("HLE Audio ОШИБКА: Не удалось загрузить " + path + ". Причина: " + e.getMessage());
        }
    }

    public void audioPlay(int ptrId) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null && !player.isPlaying()) {
            player.start();
        }
    }

    public void audioPause(int ptrId) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null && player.isPlaying()) {
            player.pause();
        }
    }

    public void audioSetLooping(int ptrId, boolean looping) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null) {
            player.setLooping(looping);
        }
    }

    public boolean audioIsPlaying(int ptrId) {
        MediaPlayer player = audioPlayers.get(ptrId);
        return player != null && player.isPlaying();
    }

    public void audioRelease(int ptrId) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null) {
            player.release();
            audioPlayers.remove(ptrId);
        }
    }

    public void audioStop(int ptrId) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null) {
            if (player.isPlaying()) {
                player.pause();
            }
            player.seekTo(0);
        }
    }

    public void audioSetVolume(int ptrId, float volume) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null) {
            player.setVolume(volume, volume);
        }
    }

    public float audioGetDuration(int ptrId) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null) {
            return player.getDuration() / 1000.0f;
        }
        return 0.0f;
    }

    public float audioGetCurrentTime(int ptrId) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null) {
            return player.getCurrentPosition() / 1000.0f;
        }
        return 0.0f;
    }

    public void audioSetCurrentTime(int ptrId, float time) {
        MediaPlayer player = audioPlayers.get(ptrId);
        if (player != null) {
            player.seekTo((int)(time * 1000.0f));
        }
    }

    // Декодирует сжатый аудиофайл (m4a/AAC, mp3, ogg) в 16-битный PCM.
    // Формат ответа: [0..3] частота, [4..7] число каналов, дальше сам PCM (всё little-endian).
    public byte[] decodeAudioFileJava(String path) {
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(path);
        } catch (Exception e) {
            addLogFromNative("HLE_AUDIO_ERROR: MediaExtractor не открыл " + path + ": " + e);
            extractor.release();
            return null;
        }
        return decodeWithExtractor(extractor, path);
    }

    // Звук внутри .ipa: архив без сжатия, поэтому дорожка лежит непрерывным срезом
    // и MediaExtractor читает её прямо из архива, без распаковки во временный файл.
    public byte[] decodeAudioRangeJava(String archivePath, long offset, long length) {
        MediaExtractor extractor = new MediaExtractor();
        java.io.RandomAccessFile raf = null;
        try {
            raf = new java.io.RandomAccessFile(archivePath, "r");
            extractor.setDataSource(raf.getFD(), offset, length);
        } catch (Exception e) {
            addLogFromNative("HLE_AUDIO_ERROR: MediaExtractor не открыл срез " + archivePath
                             + " [" + offset + ", " + length + "]: " + e);
            extractor.release();
            try { if (raf != null) raf.close(); } catch (Exception ignored) {}
            return null;
        }
        try {
            return decodeWithExtractor(extractor, archivePath + "[" + offset + "]");
        } finally {
            try { raf.close(); } catch (Exception ignored) {}
        }
    }

    private byte[] decodeWithExtractor(MediaExtractor extractor, String path) {
        MediaCodec codec = null;
        try {
            int track = -1;
            MediaFormat format = null;
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat f = extractor.getTrackFormat(i);
                if (f.getString(MediaFormat.KEY_MIME).startsWith("audio/")) { track = i; format = f; break; }
            }
            if (track < 0) {
                addLogFromNative("HLE_AUDIO_ERROR: в файле нет аудиодорожки: " + path);
                return null;
            }
            extractor.selectTrack(track);

            int sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE);
            int channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT);

            codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME));
            codec.configure(format, null, null, 0);
            codec.start();

            ByteArrayOutputStream pcm = new ByteArrayOutputStream(1 << 16);
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            boolean inputDone = false, outputDone = false;

            while (!outputDone) {
                if (!inputDone) {
                    int inIdx = codec.dequeueInputBuffer(10000);
                    if (inIdx >= 0) {
                        ByteBuffer inBuf = codec.getInputBuffer(inIdx);
                        int size = extractor.readSampleData(inBuf, 0);
                        if (size < 0) {
                            codec.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputDone = true;
                        } else {
                            codec.queueInputBuffer(inIdx, 0, size, extractor.getSampleTime(), 0);
                            extractor.advance();
                        }
                    }
                }
                int outIdx = codec.dequeueOutputBuffer(info, 10000);
                if (outIdx >= 0) {
                    if (info.size > 0) {
                        ByteBuffer outBuf = codec.getOutputBuffer(outIdx);
                        byte[] chunk = new byte[info.size];
                        outBuf.position(info.offset);
                        outBuf.get(chunk);
                        pcm.write(chunk);
                    }
                    codec.releaseOutputBuffer(outIdx, false);
                    if ((info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) outputDone = true;
                } else if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    // Настоящие параметры известны только после старта декодера
                    MediaFormat out = codec.getOutputFormat();
                    sampleRate = out.getInteger(MediaFormat.KEY_SAMPLE_RATE);
                    channels = out.getInteger(MediaFormat.KEY_CHANNEL_COUNT);
                }
            }

            byte[] body = pcm.toByteArray();
            byte[] result = new byte[8 + body.length];
            for (int i = 0; i < 4; i++) result[i] = (byte)((sampleRate >> (i * 8)) & 0xFF);
            for (int i = 0; i < 4; i++) result[4 + i] = (byte)((channels >> (i * 8)) & 0xFF);
            System.arraycopy(body, 0, result, 8, body.length);
            addLogFromNative("HLE_AUDIO: декодирован " + path + " -> " + body.length + " байт PCM, " + sampleRate + " Гц, каналов: " + channels);
            return result;
        } catch (Exception e) {
            addLogFromNative("HLE_AUDIO_ERROR: не удалось декодировать " + path + ": " + e);
            return null;
        } finally {
            if (codec != null) { try { codec.stop(); } catch (Exception ignored) {} codec.release(); }
            extractor.release();
        }
    }

    // ==========================================
    // OPENAL NATIVE BRIDGES
    // ==========================================
    public void alBufferDataJava(int bufferId, int format, byte[] data, int freq) {
        alBuffers.put(bufferId, data);
        alBufferFreqs.put(bufferId, freq);
        // Форматы: AL_FORMAT_MONO8 = 0x1100, AL_FORMAT_MONO16 = 0x1101, AL_FORMAT_STEREO8 = 0x1102, AL_FORMAT_STEREO16 = 0x1103
        int channels = (format == 0x1102 || format == 0x1103) ? AudioFormat.CHANNEL_OUT_STEREO : AudioFormat.CHANNEL_OUT_MONO;
        int bitDepth = (format == 0x1101 || format == 0x1103) ? AudioFormat.ENCODING_PCM_16BIT : AudioFormat.ENCODING_PCM_8BIT;
        alBufferChannels.put(bufferId, channels | (bitDepth << 16));
        addLogFromNative("HLE OpenAL: Загружен буфер " + bufferId + ", размер: " + data.length + " байт, freq: " + freq);
    }

    public void alSourceiJava(int sourceId, int param, int value) {
        if (param == 0x1009) { // AL_BUFFER
            alSourceToBuffer.put(sourceId, value);
        }
    }

    public void alSourcePlayJava(int sourceId) {
        Integer bufferId = alSourceToBuffer.get(sourceId);
        if (bufferId == null) return;
        
        byte[] pcm = alBuffers.get(bufferId);
        Integer freq = alBufferFreqs.get(bufferId);
        Integer formatPack = alBufferChannels.get(bufferId);
        
        if (pcm == null || freq == null || formatPack == null) return;

        int channels = formatPack & 0xFFFF;
        int bitDepth = formatPack >> 16;

        AudioTrack track = alSourceTracks.get(sourceId);
        if (track != null) {
            track.stop();
            track.release();
        }

        try {
            int minSize = AudioTrack.getMinBufferSize(freq, channels, bitDepth);
            int trackSize = Math.max(minSize, pcm.length);

            track = new AudioTrack(AudioManager.STREAM_MUSIC, freq, channels, bitDepth, trackSize, AudioTrack.MODE_STATIC);
            track.write(pcm, 0, pcm.length);
            track.play();
            alSourceTracks.put(sourceId, track);
        } catch (Exception e) {
            addLogFromNative("HLE OpenAL ОШИБКА: " + e.getMessage());
        }
    }

    public void alSourceStopJava(int sourceId) {
        AudioTrack track = alSourceTracks.get(sourceId);
        if (track != null && track.getPlayState() == AudioTrack.PLAYSTATE_PLAYING) {
            track.stop();
        }
    }

    public void alSourcefJava(int sourceId, int param, float value) {
        if (param == 0x100A) { // AL_GAIN (Громкость)
            AudioTrack track = alSourceTracks.get(sourceId);
            if (track != null) {
                track.setVolume(value);
            }
        }
    }

    // ==========================================
    // VIDEO NATIVE CALLS
    // ==========================================
    public void videoInit(int ptrId, String path) {
        runOnUiThread(() -> {
            activeVideoPtrId = ptrId;
            if (getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean("novideo_cmd", false)) {
                addLogFromNative("HLE Video: Инициализация " + path + " пропущена (-novideo)");
                return;
            }
            videoView.setVideoPath(path);
            addLogFromNative("HLE Video: Успешно инициализирован " + path);
        });
    }

    public void videoPlay(int ptrId) {
        runOnUiThread(() -> {
            activeVideoPtrId = ptrId;
            if (getSharedPreferences("DamnPrefs", MODE_PRIVATE).getBoolean("novideo_cmd", false)) {
                new Thread(() -> {
                    addLogFromNative("Java Thread: Пропуск видео (команда -novideo)...");
                    onVideoFinishedNative(ptrId);
                }).start();
                return;
            }
            videoView.setVisibility(View.VISIBLE); // ФИКС: Явно возвращаем Surface
            videoContainer.setVisibility(View.VISIBLE);
            skipButton.setVisibility(View.GONE);
            videoView.start();
        });
    }

    public void videoStop(int ptrId) {
        runOnUiThread(this::stopVideo);
    }

    private void stopVideo() {
        if (videoContainer != null && videoContainer.getVisibility() == View.VISIBLE) {
            addLogFromNative("Java UI: Скрытие видеоплеера и освобождение Surface...");
            videoView.stopPlayback(); // Останавливаем и освобождаем буфер
            videoView.setVisibility(View.GONE); // Принудительно гасим окно VideoView
            videoContainer.setVisibility(View.GONE);
            if (hideSkipRunnable != null) videoHandler.removeCallbacks(hideSkipRunnable);
            
            int ptrId = activeVideoPtrId;
            new Thread(() -> {
                addLogFromNative("Java Thread: Вызов onVideoFinishedNative...");
                onVideoFinishedNative(ptrId);
            }).start();
        }
    }

    // Сообщения самого враппера (шапка запуска, выбранная игра, ошибки) не относятся
    // ни к одной игровой категории: их режет только полное выключение лога.
    private void addLog(String msg) {
        if (buildLogMask() == 0) return;

        logTextView.append(msg + "\n");
        scrollView.post(() -> scrollView.fullScroll(View.FOCUS_DOWN));
        Log.d("DamnWrapper32_ARMv7", msg);
        try {
            java.io.FileOutputStream fos = new java.io.FileOutputStream(WORK_DIR + "damn32_log.txt", true);
            fos.write((msg + "\n").getBytes());
            fos.close();
        } catch (Exception e) {}
    }


    @Override public void surfaceCreated(SurfaceHolder holder) { onSurfaceCreated(holder.getSurface()); }
    @Override public void surfaceChanged(SurfaceHolder holder, int f, int w, int h) { onSurfaceChanged(w, h); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {}

    @Override
    protected void onResume() {
        super.onResume();
        if (sensorManager != null) {
            if (accelerometer != null) sensorManager.registerListener(this, accelerometer, SensorManager.SENSOR_DELAY_GAME);
            if (gyroscope != null) sensorManager.registerListener(this, gyroscope, SensorManager.SENSOR_DELAY_GAME);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (sensorManager != null) {
            sensorManager.unregisterListener(this);
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (!isRendering) return;
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            // iOS UIAccelerometer ожидает значения в G, но вектор гравитации строго инвертирован относительно Android
            onSensorChangedNative(1, -event.values[0] / 9.81f, -event.values[1] / 9.81f, -event.values[2] / 9.81f);
        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            // iOS CMMotionManager ожидает значения в рад/с (как и дает Android)
            onSensorChangedNative(4, event.values[0], event.values[1], event.values[2]);
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    private View createWallpaperView(Context context, File file) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                android.graphics.ImageDecoder.Source source = android.graphics.ImageDecoder.createSource(file);
                android.graphics.drawable.Drawable drawable = android.graphics.ImageDecoder.decodeDrawable(source, new android.graphics.ImageDecoder.OnHeaderDecodedListener() {
                    @Override
                    public void onHeaderDecoded(android.graphics.ImageDecoder decoder, android.graphics.ImageDecoder.ImageInfo info, android.graphics.ImageDecoder.Source src) {
                        decoder.setAllocator(android.graphics.ImageDecoder.ALLOCATOR_SOFTWARE);
                    }
                });
                ImageView iv = new ImageView(context);
                iv.setScaleType(ImageView.ScaleType.CENTER_CROP);
                iv.setImageDrawable(drawable);
                if (drawable instanceof android.graphics.drawable.AnimatedImageDrawable) {
                    ((android.graphics.drawable.AnimatedImageDrawable) drawable).start();
                }
                return iv;
            }
        } catch (Exception e) {}
        
        FallbackWallpaperView fwv = new FallbackWallpaperView(context, file);
        if (fwv.isValid()) return fwv;
        return null;
    }

    private class FallbackWallpaperView extends View {
        private android.graphics.Movie mMovie;
        private Bitmap mBitmap;
        private long mMovieStart;

        public FallbackWallpaperView(Context context, File file) {
            super(context);
            try {
                byte[] bytes = new byte[(int) file.length()];
                FileInputStream fis = new FileInputStream(file);
                fis.read(bytes);
                fis.close();
                
                if (bytes.length > 3 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') {
                    mMovie = android.graphics.Movie.decodeByteArray(bytes, 0, bytes.length);
                    setLayerType(View.LAYER_TYPE_SOFTWARE, null);
                }
                if (mMovie == null || mMovie.duration() == 0) {
                    mMovie = null;
                    mBitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.length);
                }
            } catch (Exception e) {}
        }
        
        public boolean isValid() { return mMovie != null || mBitmap != null; }

        @Override
        protected void onDraw(android.graphics.Canvas canvas) {
            if (mMovie != null) {
                long now = android.os.SystemClock.uptimeMillis();
                if (mMovieStart == 0) mMovieStart = now;
                int dur = mMovie.duration();
                if (dur == 0) dur = 1000;
                int relTime = (int)((now - mMovieStart) % dur);
                mMovie.setTime(relTime);
                float scaleX = (float) getWidth() / mMovie.width();
                float scaleY = (float) getHeight() / mMovie.height();
                float scale = Math.max(scaleX, scaleY);
                canvas.save();
                float dx = (getWidth() - mMovie.width() * scale) / 2f;
                float dy = (getHeight() - mMovie.height() * scale) / 2f;
                canvas.translate(dx, dy);
                canvas.scale(scale, scale);
                mMovie.draw(canvas, 0, 0);
                canvas.restore();
                invalidate();
            } else if (mBitmap != null) {
                float scaleX = (float) getWidth() / mBitmap.getWidth();
                float scaleY = (float) getHeight() / mBitmap.getHeight();
                float scale = Math.max(scaleX, scaleY);
                float dx = (getWidth() - mBitmap.getWidth() * scale) / 2f;
                float dy = (getHeight() - mBitmap.getHeight() * scale) / 2f;
                android.graphics.Matrix matrix = new android.graphics.Matrix();
                matrix.postScale(scale, scale);
                matrix.postTranslate(dx, dy);
                canvas.drawBitmap(mBitmap, matrix, null);
            }
        }
    }
}
