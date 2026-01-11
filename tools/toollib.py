from pathlib import Path
class IllegalPathException(Exception):
    def __init__(self, message):
        self.message = message
        super().__init__(self.message)
def file_path(string):
    p = Path(string)
    if p.parent.exists() and p.parent.is_dir():
        return p
    else:
        raise NotADirectoryError(string)

def dir_path(string):
    p = Path(string)
    if p.exists() and p.is_dir():
        return p
    else:
        raise NotADirectoryError(string)