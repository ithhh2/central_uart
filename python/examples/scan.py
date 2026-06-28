from ble_at import BleAtHost


def main() -> None:
    ble = BleAtHost("COM15")
    try:
        for result in ble.scan(duration_s=3):
            print(result)
    finally:
        ble.close()


if __name__ == "__main__":
    main()
