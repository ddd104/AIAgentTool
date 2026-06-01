# Blueprint Patch DSL

## Document shape

```yaml
target: /Game/Blueprints/BP_Door
operations:
  - op: ensure_variable
    name: IsOpen
    type: bool
    default: false
```

## Supported operations

### ensure_variable

```yaml
- op: ensure_variable
  name: IsOpen
  type: bool
  default: false
```

Supported primitive type aliases:

- `bool`
- `float`
- `int`
- `string`
- `name`
- `text`
- object/class paths in future extension

### ensure_function

```yaml
- op: ensure_function
  name: Interact
```

### add_node

```yaml
- op: add_node
  graph: Interact
  id: branch
  type: Branch
```

Supported `type` values in this reference implementation:

- `Branch`
- `GetVariable`
- `SetVariable`
- `CallFunction`
- `CustomEvent`

### connect_exec

```yaml
- op: connect_exec
  from: Entry.Then
  to: branch.Execute
```

### connect_data

```yaml
- op: connect_data
  from: get_is_open.IsOpen
  to: branch.Condition
```

### set_pin_default

```yaml
- op: set_pin_default
  target: print.InString
  value: Hello
```

### remove_node

```yaml
- op: remove_node
  graph: EventGraph
  id: old_node
```

### configure_interactable_actor

```yaml
- op: configure_interactable_actor
  mesh: /Engine/BasicShapes/Cube.Cube
  key: E
  openAngle: 90
  duration: 0.8
  track: Alpha
  rotationComponent: DoorPivot
  promptText: Press E
```

`configure_interactable_actor` uses a normalized Timeline float track (`0.0 -> 1.0`) and lerps that alpha to `openAngle`, so the Timeline output is not accidentally treated as degrees twice.

### layout_blueprint_graph

```yaml
- op: layout_blueprint_graph
  graph: EventGraph
  avoidOverlap: true
  nodes:
    - node: EventGraph:K2Node_InputKey_0
      x: -1180
      y: 560
    - node: EventGraph:DoorTimeline
      x: 180
      y: 560
```

Use `layout_blueprint_graph` after structural graph edits when node positions are generated or repaired. Node refs can be GUIDs, object names, titles, or `GraphName:NodeRef`.

### configure_timeline

```yaml
- op: configure_timeline
  graph: EventGraph
  name: MotionTimeline
  id: motion_timeline
  x: 200
  y: 500
  length: 1.0
  loop: false
  autoplay: false
  tracks:
    - type: float
      name: Alpha
      keys:
        - { time: 0.0, value: 0.0, interp: linear }
        - { time: 1.0, value: 1.0, interp: linear }
    - type: vector
      name: Offset
      keys:
        - { time: 0.0, value: [0, 0, 0] }
        - { time: 1.0, value: [100, 0, 0] }
    - type: event
      name: Pulse
      keys:
        - { time: 0.25 }
        - { time: 0.75 }
    - type: color
      name: Tint
      keys:
        - { time: 0.0, value: [1, 0, 0, 1] }
        - { time: 1.0, value: [0, 0.25, 1, 1] }
```

`configure_timeline` creates or updates the Timeline template and its node, then reconstructs the node so float, vector, event, and color output pins are available for later `connect_*` operations. Typed arrays are also accepted: `floatTracks`, `vectorTracks`, `eventTracks`, `colorTracks`, and `linearColorTracks`.

## Node references

`Entry` is a virtual id for function entry / event entry when the graph was created by `ensure_function`.

Other ids are local to a single patch execution.
