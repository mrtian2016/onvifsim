"""PTZ 的裸 SOAP 调用。

用裸 SOAP 而不是 onvif-zeep，是因为好几条 PTZ quirk 就是要吐
onvif-zeep 解析不了的东西（C5 畸形响应、C9 裸字符串返回值）。
"""

from . import soap


def get_nodes(url, user, password, **kwargs):
    return soap.findall(soap.call(url, "<tptz:GetNodes/>", user, password, **kwargs),
                        "PTZNode")


def get_node(url, user, password, node_token, **kwargs):
    body = ("<tptz:GetNode><tptz:NodeToken>%s</tptz:NodeToken></tptz:GetNode>"
            % node_token)
    return soap.find(soap.call(url, body, user, password, **kwargs), "PTZNode")


def get_configurations(url, user, password, **kwargs):
    return soap.findall(
        soap.call(url, "<tptz:GetConfigurations/>", user, password, **kwargs),
        "PTZConfiguration")


def continuous_move(url, user, password, profile_token, pan=0.0, tilt=0.0, zoom=0.0,
                    timeout=None, **kwargs):
    velocity = '<tt:PanTilt x="%s" y="%s"/>' % (pan, tilt)
    if zoom:
        velocity += '<tt:Zoom x="%s"/>' % zoom
    timeout_xml = ("<tptz:Timeout>%s</tptz:Timeout>" % timeout) if timeout else ""
    body = ("<tptz:ContinuousMove><tptz:ProfileToken>%s</tptz:ProfileToken>"
            "<tptz:Velocity>%s</tptz:Velocity>%s</tptz:ContinuousMove>"
            % (profile_token, velocity, timeout_xml))
    return soap.call(url, body, user, password, **kwargs)


def stop(url, user, password, profile_token, pan_tilt=True, zoom=True, **kwargs):
    body = ("<tptz:Stop><tptz:ProfileToken>%s</tptz:ProfileToken>"
            "<tptz:PanTilt>%s</tptz:PanTilt><tptz:Zoom>%s</tptz:Zoom></tptz:Stop>"
            % (profile_token, str(pan_tilt).lower(), str(zoom).lower()))
    return soap.call(url, body, user, password, **kwargs)


def get_status(url, user, password, profile_token, **kwargs):
    """GetStatus → ``{"pan":..., "tilt":..., "zoom":..., "moveStatus":...}``。"""
    body = ("<tptz:GetStatus><tptz:ProfileToken>%s</tptz:ProfileToken>"
            "</tptz:GetStatus>" % profile_token)
    root = soap.call(url, body, user, password, **kwargs)
    position = soap.find(root, "Position")
    pan_tilt = soap.find(position, "PanTilt")
    zoom = soap.find(position, "Zoom")
    move_status = soap.find(root, "MoveStatus")
    return {
        "pan": float(soap.attr(pan_tilt, "x", "0") or 0),
        "tilt": float(soap.attr(pan_tilt, "y", "0") or 0),
        "zoom": float(soap.attr(zoom, "x", "0") or 0),
        "panTiltStatus": soap.text(soap.find(move_status, "PanTilt")),
        "zoomStatus": soap.text(soap.find(move_status, "Zoom")),
        "utcTime": soap.text(soap.find(root, "UtcTime")),
    }


def get_presets(url, user, password, profile_token, **kwargs):
    """GetPresets → ``[(token, name)]``。"""
    body = ("<tptz:GetPresets><tptz:ProfileToken>%s</tptz:ProfileToken>"
            "</tptz:GetPresets>" % profile_token)
    root = soap.call(url, body, user, password, **kwargs)
    presets = []
    for preset in soap.findall(root, "Preset"):
        presets.append((soap.attr(preset, "token"),
                        soap.text(soap.find(preset, "Name"))))
    return presets


def set_preset(url, user, password, profile_token, name=None, token=None, **kwargs):
    """SetPreset。返回原始的 ``requests.Response`` —— C9 要看返回值的形态。"""
    extra = ""
    if name is not None:
        extra += "<tptz:PresetName>%s</tptz:PresetName>" % name
    if token is not None:
        extra += "<tptz:PresetToken>%s</tptz:PresetToken>" % token
    body = ("<tptz:SetPreset><tptz:ProfileToken>%s</tptz:ProfileToken>%s"
            "</tptz:SetPreset>" % (profile_token, extra))
    return soap.post(url, body, user, password, **kwargs)


def goto_preset(url, user, password, profile_token, preset_token, **kwargs):
    body = ("<tptz:GotoPreset><tptz:ProfileToken>%s</tptz:ProfileToken>"
            "<tptz:PresetToken>%s</tptz:PresetToken></tptz:GotoPreset>"
            % (profile_token, preset_token))
    return soap.call(url, body, user, password, **kwargs)


def remove_preset(url, user, password, profile_token, preset_token, **kwargs):
    body = ("<tptz:RemovePreset><tptz:ProfileToken>%s</tptz:ProfileToken>"
            "<tptz:PresetToken>%s</tptz:PresetToken></tptz:RemovePreset>"
            % (profile_token, preset_token))
    return soap.call(url, body, user, password, **kwargs)


def profile_ptz_config_tokens(profiles):
    """每条 profile 挂的 PTZConfiguration token（没挂就是 None）。"""
    tokens = []
    for profile in profiles:
        config = soap.find(profile, "PTZConfiguration")
        tokens.append(soap.attr(config, "token") if config is not None else None)
    return tokens


def spaces_of(node):
    """PTZNode 里 SupportedPTZSpaces 下所有 URI 的个数。"""
    spaces = soap.find(node, "SupportedPTZSpaces")
    if spaces is None:
        return []
    return [soap.text(uri) for uri in soap.findall(spaces, "URI")]


def ranges_of(node):
    """SupportedPTZSpaces 里的 (Min, Max) 对，用来验 C4。"""
    spaces = soap.find(node, "SupportedPTZSpaces")
    pairs = []
    for x_range in soap.findall(spaces, "XRange") + soap.findall(spaces, "YRange"):
        minimum = soap.text(soap.find(x_range, "Min"))
        maximum = soap.text(soap.find(x_range, "Max"))
        if minimum is not None and maximum is not None:
            pairs.append((float(minimum), float(maximum)))
    return pairs
