extends PanelContainer

var players_root: Node3D
#var music = AudioStreamMP3.load_from_file("res://sound/BeMax - Sea of Tranquility(Radio Edit).mp3")
var mic = AudioStreamMicrophone.new()

# 参数定义
var params = [
    {"name": "air_absorption", "type": "bool", "default": true},
    {"name": "air_absorption_high", "type": "float", "default": 0.5, "min": 0.0, "max": 10.0, "step": 0.01},
    {"name": "air_absorption_low", "type": "float", "default": 0.9, "min": 0.0, "max": 10.0, "step": 0.01},
    {"name": "air_absorption_mid", "type": "float", "default": 0.7, "min": 0.0, "max": 10.0, "step": 0.01},
    {"name": "air_absorption_model", "type": "enum", "default": 0, "items": ["Default", "Exponential"]},
    {"name": "ambisonics_order", "type": "int", "default": 1, "min": 0, "max": 4},
    {"name": "distance_attenuation", "type": "bool", "default": true},
    {"name": "max_reflection_distance", "type": "float", "default": 10000.0, "min": 0.0, "max": 20000.0, "step": 10.0},
    {"name": "min_attenuation_distance", "type": "float", "default": 0.0, "min": 0.0, "max": 10000.0, "step": 1.0},
    {"name": "occlusion", "type": "bool", "default": true},
    {"name": "occlusion_radius", "type": "float", "default": 4.0, "min": 0.0, "max": 50.0, "step": 0.1},
    {"name": "occlusion_samples", "type": "int", "default": 32, "min": 0, "max": 128},
    {"name": "reflection", "type": "bool", "default": true},
    {"name": "transmission_rays", "type": "int", "default": 16, "min": 0, "max": 128}
]

func _ready():
    # 查找 Door House 节点
    players_root = get_node_or_null("../../../Door House")
    if not players_root:
        print("Error: Cannot find Door House node from ", get_path())
    
    # 创建滚动容器
    var scroll = ScrollContainer.new()
    scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
    scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
    scroll.set_anchors_preset(Control.PRESET_FULL_RECT)
    add_child(scroll)
    
    var vbox = VBoxContainer.new()
    vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    scroll.add_child(vbox)
    
    var header = HBoxContainer.new()
    header.size_flags_horizontal = Control.SIZE_EXPAND_FILL

    var title = Label.new()
    title.text = "Steam Audio Parameters"
    title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
    header.add_child(title)

    var close_btn = Button.new()
    close_btn.text = "Close"
    close_btn.connect("pressed", Callable(self , "_on_close_pressed"))
    header.add_child(close_btn)

    vbox.add_child(header)
    
    for p in params:
        var hbox = HBoxContainer.new()
        vbox.add_child(hbox)
        
        var label = Label.new()
        label.text = p["name"]
        label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
        hbox.add_child(label)
        
        if p["type"] == "bool":
            var check = CheckButton.new()
            check.button_pressed = p["default"]
            check.connect("toggled", Callable(self , "_on_param_changed").bind(p["name"]))
            hbox.add_child(check)
            _on_param_changed(p["default"], p["name"])
            
        elif p["type"] == "float":
            var spin = SpinBox.new()
            spin.min_value = p.get("min", 0.0)
            spin.max_value = p.get("max", 100.0)
            spin.step = p.get("step", 0.1)
            spin.value = p["default"]
            spin.connect("value_changed", Callable(self , "_on_param_changed").bind(p["name"]))
            hbox.add_child(spin)
            _on_param_changed(p["default"], p["name"])
            
        elif p["type"] == "int":
            var spin = SpinBox.new()
            spin.min_value = p.get("min", 0)
            spin.max_value = p.get("max", 100)
            spin.step = 1
            spin.value = p["default"]
            spin.connect("value_changed", Callable(self , "_on_param_changed").bind(p["name"]))
            hbox.add_child(spin)
            _on_param_changed(p["default"], p["name"])
            
        elif p["type"] == "enum":
            var opt = OptionButton.new()
            if p.has("items"):
                for item in p["items"]:
                    opt.add_item(item)
            opt.selected = p["default"]
            opt.connect("item_selected", Callable(self , "_on_param_changed").bind(p["name"]))
            hbox.add_child(opt)
            _on_param_changed(p["default"], p["name"])

    # 默认隐藏面板（作为叠加层），并连接父容器中的打开按钮（如果存在）
    hide()
    var parent_ctrl = get_parent()
    if parent_ctrl:
        var open_btn = parent_ctrl.get_node_or_null("ParamsButton")
        if open_btn:
            open_btn.connect("pressed", Callable(self , "_on_open_pressed"))

func _on_open_pressed():
    show()

func _on_close_pressed():
    hide()

func _on_param_changed(value, param_name):
    if not players_root:
        return
    
    #for i in range(1, 2):
        #var node_name = "SteamAudioPlayer" + str(i)
        #var player = players_root.get_node_or_null(node_name)
        #if player:
            #player.set(param_name, value)
        #player.play_stream(music)
        #await get_tree().create_timer(0.1).timeout
        #player.play_stream(mic)
