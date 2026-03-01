extends Node3D

@onready var requester = Requester.new()
@onready var configmanager = ConfigManager.new()
@onready var computermamager = ComputerManager.new()
@onready var moonlightstreamcore = MoonlightStreamCore.new()

# UI References
@onready var ui_layer = $UILayer
@onready var settings_panel = $UILayer/SettingsPanel
@onready var settings_btn = $UILayer/SettingsButton
@onready var close_btn = $UILayer/SettingsPanel/VBoxContainer/HBoxContainer/CloseButton
@onready var scroll_container = $UILayer/SettingsPanel/VBoxContainer/ScrollContainer
@onready var grid_container = $UILayer/SettingsPanel/VBoxContainer/ScrollContainer/GridContainer
@onready var line_edit_ip = grid_container.get_node("LineEdit")
@onready var line_edit_bitrate = grid_container.get_node("LineEdit2")
@onready var texture_rect_app = grid_container.get_node("TextureRect")

# 3D References
@onready var player = $Player
@onready var head = $Player/Head
@onready var screen_mesh = $TVModel/ScreenMesh
@onready var stream_viewport = $StreamViewport
@onready var stream_texture = $StreamViewport/StreamTexture
@onready var audio_left = $TVModel/AudioLeft
@onready var audio_right = $TVModel/AudioRight

# Exported list of AudioStreamPlayer3D nodes (editable in Inspector)
# Use string node paths so the editor won't attempt to construct NodePath/Node objects at parse time
# @export var audio_player_paths: Array = ["TVModel/AudioLeft", "TVModel/AudioRight"]
@export var audio_players: Array[AudioStreamPlayer3D]
# Settings
var video_enabled: bool = true
var audio_enabled: bool = true
var input_enabled: bool = true
var godot_audio_enabled: bool = true # Controls if we play audio through Godot nodes

# Movement Settings
const SPEED = 5.0
const JUMP_VELOCITY = 4.5
const MOUSE_SENSITIVITY = 0.003

# State
enum PlayMode {
    WALK,
    STREAM_CONTROL
}
var current_mode = PlayMode.WALK

func _ready() -> void:
    # --- ROBUST TEXTURE ASSIGNMENT ---
    # 1. Ensure the mesh has a unique material override
    var mat = screen_mesh.get_active_material(0)
    if not mat or not (mat is StandardMaterial3D):
        mat = StandardMaterial3D.new()
        screen_mesh.material_override = mat

    # 2. Configure material for screen display (Unshaded = self-illuminated)
    mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    mat.albedo_color = Color.WHITE
    mat.cull_mode = BaseMaterial3D.CULL_DISABLED # Ensure double-sided rendering
    
    # 3. Assign the Viewport Texture to the Material
    mat.albedo_texture = stream_viewport.get_texture()
    print("Assigned Viewport Texture to Screen Material: ", mat.albedo_texture)

    # 4. Ensure TextureRect inside Viewport is set up correctly
    stream_texture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
    stream_texture.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
    # --------------------------------

    # --- AUDIO SETUP (Godot Audio) ---
    # Build audio_players from exported paths so user can customize nodes in inspector
    # audio_players.clear()
    # for p in audio_player_paths:
    #     if p == null:
    #         continue
    #     var np = NodePath(str(p))
    #     if has_node(np):
    #         var ap = get_node(np)
    #         audio_players.append(ap)
    #     else:
    #         print("Audio player path not found:", np)

    # Configure each audio player for audible spatial playback
    for ap in audio_players:
        if ap and ap is AudioStreamPlayer3D:
            ap.unit_size = 20.0
            ap.max_distance = 50.0
            ap.attenuation_model = AudioStreamPlayer3D.ATTENUATION_INVERSE_DISTANCE
        else:
            print("Warning: audio_players contains non-AudioStreamPlayer3D or null:", ap)
    # -------------------
    
    # Initial UI State
    settings_panel.visible = false
    Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
    
    # Connect Moonlight Signals
    computermamager.pair_completed.connect(on_pair_complete)
    moonlightstreamcore.connection_started.connect(func(): print("[Moonlight-Godot-MoonlightStreamCore]", "Connect Successfully!"))
    moonlightstreamcore.connection_terminated.connect(func(_err, msg): push_error("[Moonlight-Godot-MoonlightStreamCore]", msg))
    
    # Load Config
    if configmanager.get_hosts().size() > 0:
        line_edit_ip.text = configmanager.get_hosts()[0].localaddress
    else:
        line_edit_ip.text = "192.168.1.1"

    # Connect UI Panel Buttons
    settings_btn.pressed.connect(toggle_settings)
    close_btn.pressed.connect(toggle_settings)
    
    # Connect Stream Toggle Buttons (Manual Connection)
    connect_button_signal("ToggleVideoButton", "toggled", func(t): video_enabled = t)
    connect_button_signal("ToggleAudioButton", "toggled", func(t): audio_enabled = t)
    connect_button_signal("ToggleInputButton", "toggled", func(t): input_enabled = t)
    
    # Connect Other Buttons
    connect_button_signal("FullscreenButton", "pressed", toggle_fullscreen)
    connect_button_signal("CheckButton2", "toggled", Callable(self, "set_godot_audio_enabled")) # Godot Audio Switch
    connect_button_signal("AudioBypassButton", "pressed", toggle_audio_bypass)
    connect_button_signal("AudioPauseButton", "pressed", toggle_pause_audio_bypass)
    connect_button_signal("KeyboardButton", "pressed", toggle_virtual_keyboard)

    # Sync Godot audio enabled state to ensure players are stopped/started accordingly
    set_godot_audio_enabled(godot_audio_enabled)

func connect_button_signal(node_name: String, signal_name: String, callable: Callable):
    if grid_container.has_node(node_name):
        var node = grid_container.get_node(node_name)
        if not node.is_connected(signal_name, callable):
            node.connect(signal_name, callable)
    else:
        print("Note: Button not found: ", node_name)

func _input(event: InputEvent) -> void:
    # 1. Global Toggle for Settings (ESC)
    if event.is_action_pressed("ui_cancel"): # ESC
        toggle_settings()
        get_viewport().set_input_as_handled()
        return
    
    # 2. If Settings Panel is Open:
    if settings_panel.visible:
        # Block game input, allow UI interaction only.
        return

    # 3. If Settings Panel is Hidden:
    
    # Toggle Mode with TAB
    if event.is_action_pressed("ui_focus_next"): # TAB
        toggle_mode()
        get_viewport().set_input_as_handled()
        return

    # Handle Mode Specific Input
    if current_mode == PlayMode.WALK:
        handle_walk_input(event)
    elif current_mode == PlayMode.STREAM_CONTROL:
        handle_stream_input(event)

func toggle_settings():
    settings_panel.visible = !settings_panel.visible
    if settings_panel.visible:
        # Show Settings -> Mouse Visible
        Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
    else:
        # Hide Settings -> Restore Capture
        Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func toggle_mode():
    if current_mode == PlayMode.WALK:
        current_mode = PlayMode.STREAM_CONTROL
        # Stay captured for stream control usually, unless using absolute mouse
        print("Switched to STREAM CONTROL mode")
    else:
        current_mode = PlayMode.WALK
        print("Switched to WALK mode")

func handle_walk_input(event: InputEvent):
    # Mouse Look
    if event is InputEventMouseMotion and Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED:
        head.rotate_y(-event.relative.x * MOUSE_SENSITIVITY)
        player.rotate_y(-event.relative.x * MOUSE_SENSITIVITY) # Body rotates Y
        
        # Head (Camera) rotates X
        var camera = head.get_node("Camera3D")
        camera.rotate_x(-event.relative.y * MOUSE_SENSITIVITY)
        camera.rotation.x = clamp(camera.rotation.x, deg_to_rad(-89), deg_to_rad(89))
        head.rotation.y = 0 # Keep head aligned

    # Click to capture if somehow lost and panel is closed
    if event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
        if not settings_panel.visible and Input.get_mouse_mode() == Input.MOUSE_MODE_VISIBLE:
            Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

func handle_stream_input(event: InputEvent):
    if not input_enabled:
        return
        
    # Forward Input to Moonlight
    if event is InputEventMouseMotion:
        # Send relative motion
        moonlightstreamcore.send_mouse_move_event(int(event.relative.x), int(event.relative.y))
        
    if event is InputEventMouseButton:
        var action = MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_PRESS if event.pressed else MoonlightInput.MouseButtonAction.MOUSE_BUTTON_ACTION_RELEASE
        var button = 0
        if event.button_index == MOUSE_BUTTON_LEFT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_LEFT
        elif event.button_index == MOUSE_BUTTON_RIGHT: button = MoonlightInput.MouseButton.MOUSE_BUTTON_RIGHT
        elif event.button_index == MOUSE_BUTTON_MIDDLE: button = MoonlightInput.MouseButton.MOUSE_BUTTON_MIDDLE
        elif event.button_index == MOUSE_BUTTON_WHEEL_UP:
            # Scroll is often handled differently or via dedicated scroll event if available
            pass
        elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
            pass
        # Note: add X1/X2 buttons if needed
        
        if button != 0:
            moonlightstreamcore.send_mouse_button_event(action, button)

    if event is InputEventKey:
        var action = MoonlightInput.KEY_ACTION_DOWN_LIMIT if event.pressed else MoonlightInput.KEY_ACTION_UP_LIMIT
        var modifiers = 0
        if event.ctrl_pressed: modifiers |= MoonlightInput.MODIFIER_CTRL_BIT
        if event.shift_pressed: modifiers |= MoonlightInput.MODIFIER_SHIFT_BIT
        if event.alt_pressed: modifiers |= MoonlightInput.MODIFIER_ALT_BIT
        if event.meta_pressed: modifiers |= MoonlightInput.MODIFIER_META_BIT
        
        moonlightstreamcore.send_keyboard_event(event.keycode, action, modifiers)

func _physics_process(delta: float) -> void:
    # Stop player movement if panel is open or not in WALK mode
    if settings_panel.visible or current_mode != PlayMode.WALK:
        return
        
    # Add gravity
    if not player.is_on_floor():
        player.velocity += player.get_gravity() * delta

    # Handle Jump
    if Input.is_action_just_pressed("ui_accept") and player.is_on_floor():
        player.velocity.y = JUMP_VELOCITY

    # Get input direction
    var input_dir = Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
    # Direction relative to Player body
    var direction = (player.transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized()
    
    if direction:
        player.velocity.x = direction.x * SPEED
        player.velocity.z = direction.z * SPEED
    else:
        player.velocity.x = move_toward(player.velocity.x, 0, SPEED)
        player.velocity.z = move_toward(player.velocity.z, 0, SPEED)

    player.move_and_slide()

# --- Button Handlers ---

func toggle_fullscreen():
    var mode = DisplayServer.window_get_mode()
    if mode == DisplayServer.WINDOW_MODE_FULLSCREEN:
        DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
    else:
        DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN)

func toggle_audio_bypass():
    if moonlightstreamcore.is_native_audio_bypass_running():
        moonlightstreamcore.stop_native_audio_bypass()
        print("Native Audio Bypass Stopped")
        $UILayer/SettingsPanel/VBoxContainer/ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: OFF"
    else:
        if moonlightstreamcore.start_native_audio_bypass():
            print("Native Audio Bypass Started")
            $UILayer/SettingsPanel/VBoxContainer/ScrollContainer/GridContainer/AudioBypassButton.text = "Audio Bypass: ON"
        else:
            print("Failed to start Native Audio Bypass")

func toggle_pause_audio_bypass():
    if moonlightstreamcore.is_native_audio_bypass_paused():
        moonlightstreamcore.resume_native_audio_bypass()
        print("Native Audio Bypass Resumed")
    else:
        moonlightstreamcore.pause_native_audio_bypass()
        print("Native Audio Bypass Paused")

func toggle_virtual_keyboard():
    print("Toggle Keyboard (Not Implemented)")

func set_godot_audio_enabled(enabled: bool) -> void:
    godot_audio_enabled = enabled
    # When toggling, stop or resume playback on all assigned audio players
    for ap in audio_players:
        if not ap or not (ap is AudioStreamPlayer3D):
            continue
        if enabled:
            if ap.stream:
                ap.play()
        else:
            ap.stop()

# --- Helper Logic ---

func on_pair_complete(success, message):
    if success:
        print("[Moonlight-Godot-ComputerManager]", message)
    else:
        push_error("[Moonlight-Godot-ComputerManager]", message)

func _on_line_edit_editing_toggled(toggled_on: bool) -> void:
    if toggled_on: return
    configmanager.update_host(1, {"localaddress": line_edit_ip.text})

func test_http_requeset() -> void:
    var url = "http://httpbin.org/uuid"
    requester.request(url, "GET", PackedByteArray(), {}, {}, Callable(self , "_on_request_completed"))

func _on_request_completed(code, body, headers, err):
    print("Request result: ", code)

func test_cert_create() -> void:
    print(configmanager.get_client_keys())

func test_add_host_info() -> void:
    print("current hosts:\n", configmanager.get_hosts(), "\n")

func test_remove_host_info() -> void:
    configmanager.remove_host(1)

func test_start_pair() -> void:
    var pin = computermamager.start_pair(line_edit_ip.text)
    print("pin:", pin)

func test_cancel_pair() -> void:
    computermamager.unpair(1)

func test_get_applist() -> void:
    computermamager.get_app_list(1)

func test_get_app_texture() -> void:
    configmanager.load_config()
    var applist = configmanager.get_apps(1)
    if applist.size() > 0:
        computermamager.get_app_cover(1, applist[0]["id"], func(tex): texture_rect_app.texture = tex)

func test_connect_to_server() -> void:
    computermamager.connect_to_computer(line_edit_ip.text, 47989, func(info): print(info))

func test_establish_stream() -> void:
    # Update settings from UI one last time
    if grid_container.has_node("ToggleVideoButton"):
        video_enabled = grid_container.get_node("ToggleVideoButton").button_pressed
    if grid_container.has_node("ToggleAudioButton"):
        audio_enabled = grid_container.get_node("ToggleAudioButton").button_pressed
    if grid_container.has_node("ToggleInputButton"):
        input_enabled = grid_container.get_node("ToggleInputButton").button_pressed
    if grid_container.has_node("CheckButton2"):
        godot_audio_enabled = grid_container.get_node("CheckButton2").button_pressed

    var cfg = MoonlightStreamConfigurationResource.new()
    cfg.set_width(1920)
    cfg.set_height(1080)
    cfg.set_fps(60)
    if line_edit_bitrate.text.is_valid_float():
        cfg.set_bitrate(int(float(line_edit_bitrate.text) * 1000))
    else:
        cfg.set_bitrate(20000)
    cfg.set_audio_configuration(MoonlightStreamConfigurationResource.AUDIO_CFG_71_SURROUND)

    var add_opts = MoonlightAdditionalStreamOptions.new()
    if grid_container.has_node("OptionButton"):
        add_opts.set_video_codec(grid_container.get_node("OptionButton").get_selected_id())
    if grid_container.has_node("CheckButton"):
        add_opts.set_disable_hw_acceleration(not grid_container.get_node("CheckButton").button_pressed)
    
    # Important: These options control what Limelight/Moonlight SDK does internally.
    # If disable_audio is TRUE, we won't get audio packets.
    add_opts.set_disable_video(not video_enabled)
    add_opts.set_disable_audio(not audio_enabled)

    if video_enabled:
        moonlightstreamcore.set_render_target(stream_texture)
    
    # Start stream
    moonlightstreamcore.start_play_stream(1, 1191261554, cfg, add_opts)
    
    # --- AUDIO Handling ---
    if audio_enabled:
        # Wait a bit for stream to initialize
        await get_tree().create_timer(1.0).timeout

        var streams = moonlightstreamcore.get_audio_streams()
        # Assign streams to exported audio players in order
        for i in range(audio_players.size()):
            var ap = audio_players[i]
            if not ap or not (ap is AudioStreamPlayer3D):
                continue
            if i < streams.size():
                ap.stream = streams[i]
                if godot_audio_enabled:
                    ap.play()
                else:
                    ap.stop()
                print("Assigned audio stream to player:", ap.name)
            else:
                # No stream available for this player
                ap.stream = null
                ap.stop()
        if streams.size() == 0:
            print("No audio streams received from moonlightstreamcore")
    else:
        # If audio disabled, ensure all players are stopped
        for ap in audio_players:
            if ap and (ap is AudioStreamPlayer3D):
                ap.stop()

    # Force update Viewport texture just in case
    if video_enabled:
        var mat = screen_mesh.material_override
        if mat: mat.albedo_texture = stream_viewport.get_texture()

func test_stop_stream() -> void:
    computermamager.stop_stream(1, func(info):
        print(info)
        moonlightstreamcore.stop_play_stream()
        moonlightstreamcore.reset_audio_stream()
        if moonlightstreamcore.is_native_audio_bypass_running():
            moonlightstreamcore.stop_native_audio_bypass()
        moonlightstreamcore.reset_render_target()
        for ap in audio_players:
            if ap and (ap is AudioStreamPlayer3D):
                ap.stop()
    )

func test_pause_stream() -> void:
    moonlightstreamcore.stop_play_stream()
    moonlightstreamcore.reset_audio_stream()
    moonlightstreamcore.reset_render_target()
    for ap in audio_players:
        if ap and (ap is AudioStreamPlayer3D):
            ap.stop()

func view_data() -> void:
    if scroll_container.has_node("FileDialog"):
        scroll_container.get_node("FileDialog").visible = true
