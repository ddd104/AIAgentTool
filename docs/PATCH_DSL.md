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

## Node references

`Entry` is a virtual id for function entry / event entry when the graph was created by `ensure_function`.

Other ids are local to a single patch execution.
