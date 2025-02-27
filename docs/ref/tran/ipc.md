# IPC Transport

## DESCRIPTION

The {{i:*ipc* transport}}{{hi:*ipc*}} provides communication support between
sockets within different processes on the same host.
For POSIX platforms, this is implemented using {{i:UNIX domain sockets}}.
For Windows, this is implemented using Windows {{i:named pipes}}.
Other platforms may have different implementation strategies.

### URI Formats

#### Traditional Names

This transport uses URIs using the scheme {{i:`ipc://`}}, followed by a path
name in the file system where the socket or named pipe should be created.

> [!TIP]
> On Windows, all names are prefixed by `\\.\pipe\` and do not
> reside in the normal file system.
> On POSIX platforms, the path is taken literally, and is relative to
> the current directory, unless it begins with `/`, in which case it is
> relative to the root directory.

> [!NOTE]
> When using relative paths on POSIX systems, the address used and returned
> in properties like [`NNG_OPT_LOCADDR`] will also be relative.
> Consequently, they will only be interpreted the same by processes that have
> the same working directory.
> To ensure maximum portability and safety, absolute paths are recommended
> whenever possible.

> [!NOTE]
> If compatibility with legacy _nanomsg_ applications is required,
> then path names must not be longer than 122 bytes, including the final
> `NUL` byte.
> This is because legacy versions of _nanomsg_ cannot express URLs
> longer than 128 bytes, including the `ipc://` prefix.

#### UNIX Aliases

The {{i:`unix://`}} scheme is an alias for `ipc://` and can be used inter-changeably, but only on POSIX systems.
{{footnote:The purpose of this scheme is to support a future transport making use of `AF_UNIX`
on Windows systems, at which time it will be necessary to discriminate between the Named Pipes and the `AF_UNIX` based transports.}}

#### Abstract Names

On Linux, this transport also can support {{i:abstract sockets}}.
Abstract sockets use a URI-encoded name after the {{i:`abstract://`}} scheme, which allows arbitrary values to be conveyed
in the path, including embedded `NUL` bytes.
For example, the name `"a\0b"` would be represented as `abstract://a%00b`.

> [!TIP]
> An empty name may be used with a listener to request "auto bind" be used to select a name.
> In this case the system will allocate a free name.
> The name assigned may be retrieved using [`NNG_OPT_LOCADDR`].

Abstract names do not include the leading `NUL` byte used in the low-level socket address.

Abstract sockets do not have any representation in the file system, and are automatically freed by
the system when no longer in use.

Abstract sockets ignore socket permissions, but it is still possible to determine the credentials
of the peer with [`NNG_OPT_PEER_UID`], and similar options.
{{footnote: This property makes it important that names be chosen randomly to
prevent unauthorized access, or that checks against the peer credentials are made, or ideally, both.}}

### Socket Address

When using an [`nng_sockaddr`] structure,
the actual structure is of type [`nng_sockaddr_ipc`],
except for abstract sockets, which use [`nng_sockaddr_abstract`].

### Transport Options

The following transport options are supported by this transport,
where supported by the underlying platform.

| Option                    | Type             | Description                                                                                                        |
| ------------------------- | ---------------- | ------------------------------------------------------------------------------------------------------------------ |
| `NNG_OPT_IPC_PERMISSIONS` | `int`            | Settable on listeners before they start, this is the UNIX file mode used when creating the socket.                 |
| `NNG_OPT_LOCADDR`         | [`nng_sockaddr`] | Local socket address, either [`nng_sockaddr_ipc`] or [`nng_sockaddr_abstract`].                                    |
| `NNG_OPT_REMADDR`         | [`nng_sockaddr`] | Remote socket address, either [`nng_sockaddr_ipc`] or [`nng_sockaddr_abstract`].                                   |
| `NNG_OPT_PEER_GID`        | `int`            | Read only option, returns the group ID of the process at the other end of the socket, if platform supports it.     |
| `NNG_OPT_PEER_PID`        | `int`            | Read only option, returns the processed ID of the process at the other end of the socket, if platform supports it. |
| `NNG_OPT_PEER_UID`        | `int`            | Read only option, returns the user ID of the process at the other end of the socket, if platform supports it.      |
| `NNG_OPT_PEER_ZONEID`     | `int`            | Read only option, returns the zone ID of the process at the other end of the socket, if platform supports it.      |
| [`NNG_OPT_LISTEN_FD`]     | `int`            | Write only for listeners before they start, use the named socket for accepting (for use with socket activation).   |

### Other Configuration Parameters

On Windows systems, the security descriptor for the listener,
which can be used to control access, can be set using the function
[`nng_listener_set_security_descriptor`].

{{#include ../xref.md}}
