# APEX 软件联调合同

`GET /sim/api/v1/templates` 新增 `apex`，用原 PUT config 保存。模板为300×270×290、六个0.4mm独立喷嘴、六槽、T0..5→同号喷嘴和槽，默认PLA。配置不携带UUID；独立data-dir创建独立实例。

`GET /printer/info` 的 result 新增 model、firmware、config、hf_virtual_printer；只有实际六头六槽/逐项路由/0.4mm/指定体积的配置才报告 model="Honor Forge"，其它为"Honor Forge Virtual Printer"，不靠显示名称判断。config为以下对象，亦在objects的configfile.config中投影：

```json
{"model":"Honor Forge","firmware":"klipper","printableVolume":[300,270,290],"extruderCount":6,"nozzleDiameters":[0.4,0.4,0.4,0.4,0.4,0.4],"physicalSlotCount":6,"toolMappings":[{"tool":0,"nozzle":0,"slot":0},{"tool":1,"nozzle":1,"slot":1},{"tool":2,"nozzle":2,"slot":2},{"tool":3,"nozzle":3,"slot":3},{"tool":4,"nozzle":4,"slot":4},{"tool":5,"nozzle":5,"slot":5}],"simulation":true,"actualFilamentLoadingVerified":false}
```

server/info亦增model/firmware，保原UUID/虚拟标记。objects新增hf_virtual_printer对象，含同能力、UUID、active_tool和虚拟chamber温度；现有extruder..extruder5、toolhead仍走Moonraker对象状态。lane_data各lane兼容旧字段，新增slot、tools数组、remaining_g和actualFilamentLoadingVerified=false。料槽仅是操作者输入的仿真状态，正式transport非mock不表示实体装料被验证。

START_PRINT/PRINT_START支持EXTRUDER、BED、CHAMBER、INITIAL_TOOL；兼容EXTRUDER_TEMP/BED_TEMP/CHAMBER_TEMP。新旧同名温度同时存在必须相等；重复、非有限、越界温度/工具在更改状态前拒绝。宏选择物理工具对应喷嘴并模拟等待温度，再执行后续运动；CHAMBER模拟上限120°C。网格参数可携带但不授真实调平资格。

metadata增加sha256（实际原始保存文件字节），job/current/history记录gcode_sha256、tools_used、各tool与slot的正挤出毫米数组。未知宏保unsupported诊断，不称实体固件支持。上传不改原字节。

本机管理fault新增upload_response_lost/start_response_lost：动作成功接受后单次切断HTTP响应体；WS start同样在成功后关闭连接。故障消费后自动清除；管理权限沿原loopback Host/Origin校验。客户端不得当普通失败盲重传/重启，应查询metadata SHA、print_stats/当前任务；运行中无token重复start返回409且job_id不变。用户主动在结束后重印仍允许。

HTTP/WS start接受可选commandId（ASCII字母/数字/._-，1–128字符）。print_stats与job/history投影job_id、command_id、gcode_sha256。相同commandId绑定同filename/SHA只返回原操作，不会在结束或重启后再次启动；同token不同文件拒绝409。隔离目录持久化最多10000条收据，满时拒绝507，不淘汰旧token造成重放。客户端失回后须同command_id、新job_id、同filename/SHA且活跃状态才可核实本次接受；缺这些字段的其它固件保持unknown，不猜成功。upload失回只在metadata的size与sha256精确匹配本次原字节时确认。

启动提交先写start-attempts.json的pending意图围栏，再创建并持久当前任务，最后提交receipt及accepted意图；只有全部完成才响应ok。中途失败返回503、终止未运行流，failed或pending token再次请求及重启后均拒绝503，不重新建任务；即使已有半提交receipt也由意图状态优先阻断。查询后操作者可明确发新commandId。旧版本已成功收据没有意图记录时按原成功合同兼容。若第一步意图本身完全无法写入，任务尚未创建，不能承诺凭空生成跨重启记录；此失败明确返回未创建任务而非accepted。所有台账保留在任务隔离目录，不写用户实例。

仅在隔离实例使用，不改系统代理、hosts、防火墙或用户现有实例。此协议证明软件闭环，不证明实体固件、装料或实物色彩效果。
