extends Node3D # 如果场景的根节点不是 Node3D 或 Node2D，请更改为适当的基类

## SteamAudioPlayer 子节点的引用
var steam_audio_player: Node = null

## ----------------------------------------------------
## SteamAudioPlayer 参数，更改时将触发重新实例化
## ----------------------------------------------------

@export var air_absorption: bool = false:
    set(value):
        air_absorption = value
        _reinstance_player()

@export var air_absorption_high: float = 0.5:
    set(value):
        air_absorption_high = value
        _reinstance_player()

@export var air_absorption_low: float = 0.9:
    set(value):
        air_absorption_low = value
        _reinstance_player()

@export var air_absorption_mid: float = 0.7:
    set(value):
        air_absorption_mid = value
        _reinstance_player()

# IPLAirAbsorptionModelType 可能是一个整数枚举
@export var air_absorption_model: int = 0:
    set(value):
        air_absorption_model = value
        _reinstance_player()

@export var ambisonics_order: int = 1:
    set(value):
        ambisonics_order = value
        _reinstance_player()

@export var distance_attenuation: bool = false:
    set(value):
        distance_attenuation = value
        _reinstance_player()

@export var max_reflection_distance: float = 10000.0:
    set(value):
        max_reflection_distance = value
        _reinstance_player()

@export var min_attenuation_distance: float = 0.0:
    set(value):
        min_attenuation_distance = value
        _reinstance_player()

@export var occlusion: bool = true:
    set(value):
        occlusion = value
        _reinstance_player()

@export var occlusion_radius: float = 4.0:
    set(value):
        occlusion_radius = value
        _reinstance_player()

@export var occlusion_samples: int = 32:
    set(value):
        occlusion_samples = value
        _reinstance_player()

@export var reflection: bool = false:
    set(value):
        reflection = value
        _reinstance_player()

@export var transmission_rays: int = 16:
    set(value):
        transmission_rays = value
        _reinstance_player()

## ----------------------------------------------------

# 要管理的 SteamAudioPlayer 子节点的名称
const PLAYER_NODE_NAME = "SteamAudioPlayer"

# 节点第一次进入场景树时调用。
func _ready():
    # 仅在非编辑模式下调用，因为在 'tool' 模式下，设置器已经处理了编辑器中的更改
    if not Engine.is_editor_hint():
        _reinstance_player()

# 重新实例化 SteamAudioPlayer 子节点并应用当前属性。
func _reinstance_player():
    # 1. 如果存在，移除并释放现有的 player 节点
    if is_instance_valid(steam_audio_player):
        # 确保在编辑模式下父节点不为空
        if is_inside_tree():
            remove_child(steam_audio_player)
        steam_audio_player.queue_free()
        steam_audio_player = null
    
    # 仅在 SteamAudioPlayer 类可用时进行实例化
    if not ClassDB.can_instantiate("SteamAudioPlayer"):
        print("无法实例化 SteamAudioPlayer。请检查 Godot Steam Audio 插件是否启用且类名是否正确。")
        return
    
    # 2. 实例化一个新的 SteamAudioPlayer
    var new_player = ClassDB.instantiate("SteamAudioPlayer")
    
    if not new_player:
        return
        
    new_player.name = PLAYER_NODE_NAME
    
    # 3. 应用所有参数
    # 注意: 即使属性可能不存在，GDScript 也会尝试设置它，所以这里假设属性存在。
    #if new_player.has_method("set_air_absorption") or new_player.has_signal("air_absorption_changed") or new_player.has_user_signal("air_absorption_changed") or new_player.has_property("air_absorption"):
    if true:
        new_player.air_absorption = air_absorption
        new_player.air_absorption_high = air_absorption_high
        new_player.air_absorption_low = air_absorption_low
        new_player.air_absorption_mid = air_absorption_mid
        new_player.air_absorption_model = air_absorption_model
        new_player.ambisonics_order = ambisonics_order
        new_player.distance_attenuation = distance_attenuation
        new_player.max_reflection_distance = max_reflection_distance
        new_player.min_attenuation_distance = min_attenuation_distance
        new_player.occlusion = occlusion
        new_player.occlusion_radius = occlusion_radius
        new_player.occlusion_samples = occlusion_samples
        new_player.reflection = reflection
        new_player.transmission_rays = transmission_rays
        var mic_stream = AudioStreamMicrophone.new()
        if new_player.has_method("play_stream"):
            new_player.play_stream(mic_stream, 0.0, 0.0, 1.0)
        else:
            # Fallback for non-SteamAudio players
            new_player.stream = mic_stream
            new_player.autoplay = true
    else:
        # 这是一个检查，如果 SteamAudioPlayer 节点没有这些属性，它将打印一条警告。
        print("警告: SteamAudioPlayer 节点似乎缺少所需的 Steam Audio 属性。")


    # 4. 将新节点作为子节点添加
    add_child(new_player)
    steam_audio_player = new_player
    
    if Engine.is_editor_hint():
        print("在编辑器中重新实例化了 SteamAudioPlayer。")
    # 运行时不做任何打印，除非有错误/警告。
