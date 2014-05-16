#!perl

use 5.010;
use strict;
use warnings;

use Config;
use Getopt::Long;
use Pod::Usage;
use File::Spec;

use build::setup;
use build::auto;
use build::probe;

my $NAME    = 'moar';
my $GENLIST = 'build/gen.list';

# configuration logic

my $failed = 0;

my %args;
my %defaults;
my %config;

my @args = @ARGV;

GetOptions(\%args, qw(
    help|?
    debug:s optimize:s instrument!
    os=s shell=s toolchain=s compiler=s
    cc=s ld=s make=s has-sha has-libuv
    static use-readline has-libtommath has-libatomic_ops
    build=s host=s big-endian
    prefix=s make-install profilecalls
)) or die "See --help for further information\n";

pod2usage(1) if $args{help};

print "Welcome to MoarVM!\n\n";

$config{prefix} = File::Spec->rel2abs($args{prefix} // 'install');
# don't install to cwd, as this would clash with lib/MAST/*.nqp
if (-e 'README.markdown' && -e "$config{prefix}/README.markdown"
 && -s 'README.markdown' == -s "$config{prefix}/README.markdown") {
    die <<ENOTTOCWD;
Configuration FAIL. Installing to MoarVM root folder is not allowed.
Please specify another installation target by using --prefix=PATH.
ENOTTOCWD
}

if (-d '.git') {
    print dots("Updating submodules");
    my $msg = qx{git submodule --quiet update --init 2>&1};
    if ($? >> 8 == 0) { print "OK\n" }
    else { softfail("git error: $msg") }
}

# fiddle with flags
$args{optimize}     = 1 if not defined $args{optimize} or $args{optimize} eq "";
$args{debug}        = 3 if defined $args{debug} and $args{debug} eq "";
$args{instrument} //= 0;
$args{static}     //= 0;
$args{profilecalls} //= 0;

$args{'use-readline'}      //= 0;
$args{'big-endian'}        //= 0;
$args{'has-libtommath'}    //= 0;
$args{'has-sha'}           //= 0;
$args{'has-libuv'}         //= 0;
$args{'has-libatomic_ops'} //= 0;

# fill in C<%defaults>
if (exists $args{build} || exists $args{host}) {
    setup_cross($args{build}, $args{host});
}
else {
    setup_native($args{os} // {
        'MSWin32' => 'win32'
    }->{$^O} // $^O);
}

$config{name}   = $NAME;
$config{perl}   = $^X;
$config{config} = join ' ', map { / / ? "\"$_\"" : $_ } @args;
$config{osname} = $^O;
$config{osvers} = $Config{osvers};
$config{profilecalls} = $args{profilecalls};

# set options that take priority over all others
my @keys = qw( cc ld make );
@config{@keys} = @args{@keys};

for (keys %defaults) {
    next if /^-/;
    $config{$_} //= $defaults{$_};
}

my $VERSION = '0.0-0';
# get version
if (open(my $fh, '<', 'VERSION')) {
    $VERSION = <$fh>;
    close($fh);
}
# .git is a file and not a directory in submodule
if (-e '.git' && open(my $GIT, '-|', "git describe --tags")) {
    $VERSION = <$GIT>;
    close($GIT);
}
chomp $VERSION;
$config{version}      = $VERSION;
$config{versionmajor} = $VERSION =~ /^(\d+)/ ? $1 : 0;
$config{versionminor} = $VERSION =~ /^\d+\.(\d+)/ ? $1 : 0;
$config{versionpatch} = $VERSION =~ /^\d+\.\d+\-(\d+)/ ? $1 : 0;

# misc defaults
$config{exe}       //= '';
$config{defs}      //= [];
$config{syslibs}   //= [];
$config{usrlibs}   //= [];
$config{platform}  //= '';
$config{crossconf} //= '';
$config{dllimport} //= '';
$config{dllexport} //= '';
$config{dlllocal}  //= '';

# assume the compiler can be used as linker frontend
$config{ld}           //= $config{cc};
$config{ldout}        //= $config{ccout};
$config{ldsys}        //= $config{ldusr};
$config{ldmiscflags}  //= $config{ccmiscflags};
$config{ldoptiflags}  //= $config{ccoptiflags};
$config{lddebugflags} //= $config{ccdebugflags};
$config{ldinstflags}  //= $config{ccinstflags};

# choose between Linenoise and GNU Readline
if ($args{'use-readline'}) {
    $config{hasreadline} = 1;
    $defaults{-thirdparty}->{ln} = undef;
    unshift @{$config{usrlibs}}, 'readline';
}
else { $config{hasreadline} = 0 }

if ($args{'has-sha'}) {
    $config{shaincludedir} = '/usr/include/sha';
    $defaults{-thirdparty}->{sha} = undef;
    unshift @{$config{usrlibs}}, 'sha';
}
else { $config{shaincludedir} = '3rdparty/sha1' }

if ($args{'has-libuv'}) {
    $defaults{-thirdparty}->{uv} = undef;
    unshift @{$config{usrlibs}}, 'uv';
}

if ($args{'has-libatomic_ops'}) {
    $defaults{-thirdparty}->{lao} = undef;
#    unshift @{$config{usrlibs}}, 'atomic_ops';
}

# mangle library names
$config{ldlibs} = join ' ',
    (map { sprintf $config{ldusr}, $_; } @{$config{usrlibs}}),
    (map { sprintf $config{ldsys}, $_; } @{$config{syslibs}});
$config{ldlibs} .= ' -ltommath' if $args{'has-libtommath'};

# macro defs
$config{ccdefflags} = join ' ', map { $config{ccdef} . $_ } @{$config{defs}};

$config{ccoptiflags}  = sprintf $config{ccoptiflags},  $args{optimize} // 1 if $config{ccoptiflags}  =~ /%s/;
$config{ccdebugflags} = sprintf $config{ccdebugflags}, $args{debug}    // 3 if $config{ccdebugflags} =~ /%s/;
$config{ldoptiflags}  = sprintf $config{ldoptiflags},  $args{optimize} // 1 if $config{ldoptiflags}  =~ /%s/;
$config{lddebugflags} = sprintf $config{lddebugflags}, $args{debug}    // 3 if $config{lddebugflags} =~ /%s/;


# generate CFLAGS
my @cflags;
push @cflags, $config{ccmiscflags};
push @cflags, $config{ccoptiflags}  if $args{optimize};
push @cflags, $config{ccdebugflags} if $args{debug};
push @cflags, $config{ccinstflags}  if $args{instrument};
push @cflags, $config{ccwarnflags};
push @cflags, $config{ccdefflags};
push @cflags, $config{ccshared}     unless $args{static};
$config{cflags} = join ' ', @cflags;

# generate LDFLAGS
my @ldflags = ($config{ldmiscflags});
push @ldflags, $config{ldoptiflags}  if $args{optimize};
push @ldflags, $config{lddebugflags} if $args{debug};
push @ldflags, $config{ldinstflags}       if $args{instrument};
push @ldflags, $config{ldrpath}           unless $args{static};
$config{ldflags} = join ' ', @ldflags;

# setup library names
$config{moarlib} = sprintf $config{lib}, $NAME;
$config{moardll} = sprintf $config{dll}, $NAME;

# setup flags for shared builds
unless ($args{static}) {
    $config{objflags}  = '@ccdef@MVM_BUILD_SHARED @ccshared@';
    $config{mainflags} = '@ccdef@MVM_SHARED';
    $config{moar}      = '@moardll@';
    $config{moarinst}  = $config{shareddir};
    $config{impinst}   = $config{sharedlib},
    $config{mainlibs}  = '@lddir@. ' .
        sprintf($config{ldimp} // $config{ldusr}, $NAME);
}
else {
    $config{objflags}  = '';
    $config{mainflags} = '';
    $config{moar}      = '@moarlib@';
    $config{moarinst}  = $config{staticdir};
    $config{impinst}   = $config{staticlib};
    $config{mainlibs}  = '@moarlib@ @thirdpartylibs@ $(LDLIBS)';
}

# some toolchains generate garbage
my @auxfiles = @{ $defaults{-auxfiles} };
$config{auxclean} = @auxfiles ? '$(RM) ' . join ' ', @auxfiles : '@:';

print "OK\n";

if ($config{crossconf}) {
    build::auto::detect_cross(\%config, \%defaults);
    build::probe::static_inline_cross(\%config, \%defaults);
    build::probe::unaligned_access_cross(\%config, \%defaults);
}
else {
    build::auto::detect_native(\%config, \%defaults);
    build::probe::static_inline_native(\%config, \%defaults);
    build::probe::unaligned_access(\%config, \%defaults);
}

my $order = $config{be} ? 'big endian' : 'little endian';

# dump configuration
print "\n", <<TERM, "\n";
        make: $config{make}
     compile: $config{cc} $config{cflags}
        link: $config{ld} $config{ldflags}
        libs: $config{ldlibs}

  byte order: $order
TERM

print dots('Configuring 3rdparty libs');

my @thirdpartylibs;
my $thirdparty = $defaults{-thirdparty};

for (keys %$thirdparty) {
    my $current = $thirdparty->{$_};
    my @keys = ( "${_}lib", "${_}objects", "${_}rule", "${_}clean");

    # don't build the library (libatomic_ops can be header-only)
    unless (defined $current) {
        @config{@keys} = ("__${_}__", '', '@:', '@:');
        next;
    }

    my ($lib, $objects, $rule, $clean);

    $lib = sprintf "%s/$config{lib}",
        $current->{path},
        $current->{name};

    # C<rule> and C<build> can be used to augment all build types
    $rule  = $current->{rule};
    $clean = $current->{clean};

    # select type of build

     # dummy build - nothing to do
    if (exists $current->{dummy}) {
        $clean //= sprintf '$(RM) %s', $lib;
    }

    # use explicit object list
    elsif (exists $current->{objects}) {
        $objects = $current->{objects};
        $rule  //= sprintf '$(AR) $(ARFLAGS) @arout@$@ @%sobjects@', $_;
        $clean //= sprintf '$(RM) @%slib@ @%sobjects@', $_, $_;
    }

    # find *.c files and build objects for those
    elsif (exists $current->{src}) {
        my @sources = map { glob "$_/*.c" } @{ $current->{src} };
        my $globs   = join ' ', map { $_ . '/*@obj@' } @{ $current->{src} };

        $objects = join ' ', map { s/\.c$/\@obj\@/; $_ } @sources;
        $rule  //= sprintf '$(AR) $(ARFLAGS) @arout@$@ %s', $globs;
        $clean //= sprintf '$(RM) %s %s', $lib, $globs;
    }

    # use an explicit rule (which has already been set)
    elsif (exists $current->{rule}) {}

    # give up
    else {
        softfail("no idea how to build '$lib'");
        print dots('    continuing anyway');
    }

    @config{@keys} = ($lib, $objects // '', $rule // '@:', $clean // '@:');

    push @thirdpartylibs, $config{"${_}lib"};
}

$config{thirdpartylibs} = join ' ', @thirdpartylibs;
my $thirdpartylibs = join "\n" . ' ' x 12, sort @thirdpartylibs;

print "OK\n";

write_backend_config();

# dump 3rdparty libs we need to build
print "\n", <<TERM, "\n";
  3rdparty: $thirdpartylibs
TERM

# only this objects are needed to build, if moar is linked together with
#  the libtommath library from the system
$config{tomobjects} = '3rdparty/libtommath/bn_mp_get_long.o 3rdparty/libtommath/bn_mp_set_long.o' if $args{'has-libtommath'};

# read list of files to generate

open my $listfile, '<', $GENLIST
    or die "$GENLIST: $!\n";

my $target;
while (<$listfile>) {
    s/^\s+|\s+$//;
    next if /^#|^$/;

    $target = $_, next
        unless defined $target;

    generate($target, $_);
    $target = undef;
}

close $listfile;

# configuration completed

print "\n", $failed ? <<TERM1 : <<TERM2;
Configuration FAIL. You can try to salvage the generated Makefile.
TERM1
Configuration SUCCESS.

Type '$config{'make'}' to build and '$config{'make'} help' to see a list of
available make targets.
TERM2

if (!$failed && $args{'make-install'}) {
    system($config{make}, 'install');
}

exit $failed;

# helper functions

# fill in defaults for native builds
sub setup_native {
    my ($os) = @_;

    print dots("Configuring native build environment");

    if (!exists $::SYSTEMS{$os}) {
        softfail("unknown OS '$os'");
        print dots("    assuming POSIX userland");
        $os = 'posix';
    }

    $defaults{os} = $os;

    my ($shell, $toolchain, $compiler, $overrides) = @{$::SYSTEMS{$os}};
    $shell     = $::SHELLS{$shell};
    $toolchain = $::TOOLCHAINS{$toolchain};
    $compiler  = $::COMPILERS{$compiler};
    set_defaults($shell, $toolchain, $compiler, $overrides);

    if (exists $args{shell}) {
        $shell = $args{shell};
        hardfail("unsupported shell '$shell'")
            unless exists $::SHELLS{$shell};

        $shell = $::SHELLS{$shell};
        set_defaults($shell);
    }

    if (exists $args{toolchain}) {
        $toolchain = $args{toolchain};
        hardfail("unsupported toolchain '$toolchain'")
            unless exists $::TOOLCHAINS{$toolchain};

        $toolchain = $::TOOLCHAINS{$toolchain};
        $compiler  = $::COMPILERS{ $toolchain->{-compiler} };
        set_defaults($toolchain, $compiler);
    }

    if (exists $args{compiler}) {
        $compiler = $args{compiler};
        hardfail("unsupported compiler '$compiler'")
            unless exists $::COMPILERS{$compiler};

        $compiler  = $::COMPILERS{$compiler};

        unless (exists $args{toolchain}) {
            $toolchain = $::TOOLCHAINS{ $compiler->{-toolchain} };
            set_defaults($toolchain);
        }

        set_defaults($compiler);
    }

    my $order = $Config{byteorder};
    if ($order eq '1234' || $order eq '12345678') {
        $defaults{be} = 0;
    }
    elsif ($order eq '4321' || $order eq '87654321') {
        $defaults{be} = 1;
    }
    else {
        ::hardfail("unsupported byte order $order");
    }
}

# fill in defaults for cross builds
sub setup_cross {
    my ($build, $host) = @_;

    print dots("Configuring cross build environment");

    hardfail("both --build and --host need to be specified")
        unless defined $build && defined $host;

    my $cc        = "$host-gcc";
    my $ar        = "$host-ar";
    my $crossconf = "--build=$build --host=$host";

    for (\$build, \$host) {
        if ($$_ =~ /-(\w+)$/) {
            $$_ = $1;
            if (!exists $::SYSTEMS{$1}) {
                softfail("unknown OS '$1'");
                print dots("    assuming GNU userland");
                $$_ = 'posix';
            }
        }
        else { hardfail("failed to parse triple '$$_'") }
    }

    $defaults{os} = $host;

    $build = $::SYSTEMS{$build};
    $host  = $::SYSTEMS{$host};

    my $shell     = $::SHELLS{ $build->[0] };
    my $toolchain = $::TOOLCHAINS{gnu};
    my $compiler  = $::COMPILERS{gcc};
    my $overrides = $host->[3];

    set_defaults($shell, $toolchain, $compiler, $overrides);

    $defaults{cc}        = $cc;
    $defaults{ar}        = $ar;
    $defaults{crossconf} = $crossconf;
    $defaults{be}        = $args{'big-endian'};
}

# sets C<%defaults> from C<@_>
sub set_defaults {
    # getting the correct 3rdparty information is somewhat tricky
    my $thirdparty = $defaults{-thirdparty} // \%::THIRDPARTY;
    @defaults{ keys %$_ } = values %$_ for @_;
    $defaults{-thirdparty} = {
        %$thirdparty, map{ %{ $_->{-thirdparty} // {} } } @_
    };
}

# fill in config values
sub configure {
    my ($template) = @_;

    while ($template =~ /@(\w+)@/) {
        my $key = $1;
        unless (exists $config{$key}) {
            return (undef, "unknown configuration key '$key'\n    known keys: " .
                join(', ', sort keys %config));
        }

        $template =~ s/@\Q$key\E@/$config{$key}/;
    }

    return $template;
}

# generate files
sub generate {
    my ($dest, $src) = @_;

    print dots("Generating $dest");

    open my $srcfile, '<', $src or hardfail($!);
    open my $destfile, '>', $dest or hardfail($!);

    while (<$srcfile>) {
        my ($line, $error) = configure($_);
        hardfail($error)
            unless defined $line;

        if ($config{sh} eq 'cmd' && $dest =~ /Makefile|config\.c/) {
            # In-between slashes in makefiles need to be backslashes on Windows.
            # Double backslashes in config.c, beause these are in qq-strings.
            my $bs = $dest =~ /Makefile/ ? '\\' : '\\\\';
            $line =~ s/(\w|\.|\w\:|\$\(PREFIX\))\/(\w|\.|\*)/$1$bs$2/g;
            $line =~ s/(\w|\.|\w\:|\$\(PREFIX\))\\(\w|\.|\*)/$1$bs$2/g if $bs eq '\\\\';

            # gmake doesn't like \*
            $line =~ s/(\w|\.|\w\:|\$\(PREFIX\))\\\*/$1\\\\\*/g
                if $config{make} eq 'gmake';
        }

        print $destfile $line;
    }

    close $srcfile;
    close $destfile;

    print "OK\n";
}

# some dots
sub dots {
    my $message = shift;
    my $length = shift || 55;

    return "$message ". '.' x ($length - length $message) . ' ';
}

# fail but continue
sub softfail {
    my ($msg) = @_;
    $failed = 1;
    print "FAIL\n";
    warn "    $msg\n";
}

# fail and don't continue
sub hardfail {
    softfail(@_);
    die "\nConfiguration PANIC. A Makefile could not be generated.\n";
}

sub write_backend_config {
    $config{backendconfig} = '';
    for my $k (keys %config) {
        next if $k eq 'backendconfig';
        my $v = $config{$k};
        
        if (ref($v) eq 'ARRAY') {
            my $i = 0;
            for (@$v) {
                $config{backendconfig} .= qq/        add_entry(tc, config, "$k\[$i]", "$_");\n/;
                $i++;
            }
        }
        elsif (ref($v) eq 'HASH') {
            # should not be there
        }
        else {
            $v //= '';
            $v   =~ s/"/\\"/g;
            $v   =~ s/\n/\\\n/g;
            $config{backendconfig} .= qq/        add_entry(tc, config, "$k", "$v");\n/;
        }
    }
}


__END__

=head1 SYNOPSIS

    ./Configure.pl -?|--help

    ./Configure.pl [--os <os>] [--shell <shell>]
                   [--toolchain <toolchain>] [--compiler <compiler>]
                   [--cc <cc>] [--ld <ld>] [--make <make>]
                   [--debug] [--optimize] [--instrument]
                   [--static] [--use-readline] [--prefix]
                   [--has-libtommath] [--has-sha] [--has-libuv]
                   [--has-libatomic_ops]

    ./Configure.pl --build <build-triple> --host <host-triple>
                   [--cc <cc>] [--ld <ld>] [--make <make>]
                   [--debug] [--optimize] [--instrument]
                   [--static] [--big-endian] [--prefix]

=head1 OPTIONS

=over 4

=item -?

=item --help

Show this help information.

=item --debug

=item --no-debug

Toggle debugging flags during compile and link. Debugging is off by
default.

=item --optimize

=item --no-optimize

Toggle optimization and debug flags during compile and link. If nothing
is specified the default is to optimize.

=item --instrument

=item --no-instrument

Toggle extra instrumentation flags during compile and link; for example,
turns on Address Sanitizer when compiling with C<clang>.  Defaults to off.

=item --os <os>

Set the operating system name which you are compiling to.

Currently supported operating systems are C<posix>, C<linux>, C<darwin>,
C<openbsd>, C<netbsd>, C<freebsd>, C<solaris>, C<win32>, C<cygwin> and
C<mingw32>.

If not explicitly set, the option will be provided by the Perl runtime.
In case of unknown operating systems, a POSIX userland is assumed.

=item --shell <shell>

Currently supported shells are C<posix> and C<win32>.

=item --toolchain <toolchain>

Currently supported toolchains are C<posix>, C<gnu>, C<bsd> and C<msvc>.

=item --compiler <compiler>

Currently supported compilers are C<gcc>, C<clang> and C<cl>.

=item --cc <cc>

Explicitly set the compiler without affecting other configuration
options.

=item --ld <ld>

Explicitly set the linker without affecting other configuration
options.

=item --make <make>

Explicitly set the make tool without affecting other configuration
options.

=item --static

Build MoarVM as a static library instead of a shared one.

=item --use-readline

Disable Linenoise and try to use the system version of GNU Readline
instead.

You must not supply this flag if you create derivative work of
MoarVM - including binary packages of MoarVM itself - that you wish
to distribute under a license other than the GNU GPL.

=item --build <build-triple> --host <host-triple>

Set up cross-compilation.

=item --big-endian

Set byte order of host system in case of cross compilation. With native
builds, the byte order is auto-detected.

=item --prefix

Install files in subdirectory /bin, /lib and /include of the supplied path.
The default prefix is "install" if this option is not passed.

=item --make-install

Build and install MoarVM in addition to configuring it.

=item --has-libtommath

Link moar with the libtommath library of the system.

=item --has-sha

Build moar with the sha1 funktions from the sha library of the system.

=item --has-libuv

Link moar with the libuv library of the system.

=item --has-libatomic_ops

Disable the build of libatomic_ops.

=back
