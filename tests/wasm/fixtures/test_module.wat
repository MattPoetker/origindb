(module
  (import "env" "host_table_write"
    (func $write (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "host_table_read"
    (func $read (param i32 i32 i32 i32 i32) (result i32)))
  (import "env" "host_set_result" (func $set_result (param i32 i32)))
  (import "env" "host_abort" (func $abort (param i32)))

  (memory (export "memory") 8 1024)
  (global $heap (mut i32) (i32.const 65536))

  (data (i32.const 16) "t\00")
  (data (i32.const 32) "k1")
  (data (i32.const 48) "{\22a\22:1}")
  (data (i32.const 96) "hello")
  (data (i32.const 112) "secret\00")
  (data (i32.const 128) "boom\00")

  (func (export "origindb_alloc") (param $size i32) (result i32)
    (local $ptr i32)
    global.get $heap
    local.set $ptr
    global.get $heap
    local.get $size
    i32.add
    i32.const 7
    i32.add
    i32.const -8
    i32.and
    global.set $heap
    local.get $ptr)

  (func (export "origindb_free") (param i32))

  (func $do_write (result i32)
    (call $write (i32.const 16) (i32.const 32) (i32.const 2)
                 (i32.const 48) (i32.const 7)))

  (func $do_read (result i32)
    (local $st i32)
    (drop (call $do_write))
    (local.set $st
      (call $read (i32.const 16) (i32.const 32) (i32.const 2)
                  (i32.const 200) (i32.const 204)))
    (if (i32.ne (local.get $st) (i32.const 1))
      (then (return (i32.const -1))))
    (call $set_result (i32.load (i32.const 200)) (i32.load (i32.const 204)))
    (i32.const 0))

  (func (export "origindb_invoke")
        (param $np i32) (param $nl i32) (param $ap i32) (param $al i32)
        (result i32)
    (local $fb i32)
    (local.set $fb (i32.load8_u (local.get $np)))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 6))
                 (i32.eq (local.get $fb) (i32.const 95)))   ;; '_' -> __init
      (then (return (i32.const 0))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 9))
                 (i32.eq (local.get $fb) (i32.const 95)))   ;; '_' -> __migrate
      (then (return (call $do_write))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 1))
                 (i32.eq (local.get $fb) (i32.const 119)))  ;; 'w'
      (then (return (call $do_write))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 2))
                 (i32.eq (local.get $fb) (i32.const 114)))  ;; 'r' -> rd
      (then (return (call $do_read))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 3))
                 (i32.eq (local.get $fb) (i32.const 114)))  ;; 'r' -> res
      (then
        (call $set_result (i32.const 96) (i32.const 5))
        (return (i32.const 0))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 4))
                 (i32.eq (local.get $fb) (i32.const 108)))  ;; 'l' -> loop
      (then (loop $spin (br $spin))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 5))
                 (i32.eq (local.get $fb) (i32.const 97)))   ;; 'a' -> abort
      (then
        (drop (call $do_write))
        (call $abort (i32.const 128))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 7))
                 (i32.eq (local.get $fb) (i32.const 119)))  ;; 'w' -> wsecret
      (then
        (return (call $write (i32.const 112) (i32.const 32) (i32.const 2)
                             (i32.const 48) (i32.const 7)))))

    (if (i32.and (i32.eq (local.get $nl) (i32.const 8))
                 (i32.eq (local.get $fb) (i32.const 103)))  ;; 'g' -> growalot
      (then
        (if (i32.eq (memory.grow (i32.const 1024)) (i32.const -1))
          (then (return (i32.const -5))))
        (return (i32.const 0))))

    (i32.const -404))
)
