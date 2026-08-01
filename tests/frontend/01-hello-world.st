fn main() -> i32 {
    printf("%s\n", "Urmom");
    return 0;
}

extern fn printf(fmt: *char, ...) -> i32;
