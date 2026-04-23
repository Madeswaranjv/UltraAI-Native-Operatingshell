import helper


class Runner:
    def run(self) -> int:
        return helper.answer()


def main() -> int:
    runner = Runner()
    return runner.run()
