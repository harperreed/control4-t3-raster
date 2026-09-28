// ABOUTME: One headless Chrome with one tab per screen, driven over CDP (chromedp): 1280x800 at scale 1,
// ABOUTME: frames from Page.startScreencast as PNG, touches dispatched as real mouse events.
package browser

import (
	"context"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/chromedp/cdproto/emulation"
	"github.com/chromedp/cdproto/input"
	"github.com/chromedp/cdproto/page"
	"github.com/chromedp/chromedp"
)

// Width and Height are the panel's logical display (tt7d's PUT /api/v1/frame wants exactly this).
const Width, Height = 1280, 800

const navigateTimeout = 30 * time.Second

// Browser is the Chrome process.
type Browser struct {
	ctx         context.Context // the browser's first target; cancelled if Chrome dies
	cancel      context.CancelFunc
	allocCancel context.CancelFunc
}

// Start launches headless Chrome. chromePath "" lets chromedp search PATH. Each flag is --name or --name=value.
func Start(chromePath string, flags []string) (*Browser, error) {
	opts := append([]chromedp.ExecAllocatorOption{}, chromedp.DefaultExecAllocatorOptions[:]...)
	opts = append(opts, chromedp.WindowSize(Width, Height))
	if chromePath != "" {
		opts = append(opts, chromedp.ExecPath(chromePath))
	}
	for _, f := range flags {
		name, value, hasValue := strings.Cut(strings.TrimPrefix(f, "--"), "=")
		if hasValue {
			opts = append(opts, chromedp.Flag(name, value))
		} else {
			opts = append(opts, chromedp.Flag(name, true))
		}
	}
	allocCtx, allocCancel := chromedp.NewExecAllocator(context.Background(), opts...)
	ctx, cancel := chromedp.NewContext(allocCtx)
	if err := chromedp.Run(ctx); err != nil { // starts Chrome
		cancel()
		allocCancel()
		return nil, fmt.Errorf("starting Chrome: %w", err)
	}
	return &Browser{ctx: ctx, cancel: cancel, allocCancel: allocCancel}, nil
}

// Done is closed when Chrome exits or loses its CDP connection.
func (b *Browser) Done() <-chan struct{} { return b.ctx.Done() }

// Close asks Chrome to quit, waiting up to 5 s, then kills it.
func (b *Browser) Close() {
	ctx, cancel := context.WithTimeout(b.ctx, 5*time.Second)
	defer cancel()
	chromedp.Cancel(ctx)
	b.cancel()
	b.allocCancel()
}

// Tab is one page, one screen.
type Tab struct {
	ctx    context.Context
	cancel context.CancelFunc
}

// OpenTab opens a blank 1280x800 tab and starts its screencast. onFrame gets every
// PNG Chrome paints; it runs on chromedp's event goroutine, so it must not block.
func (b *Browser) OpenTab(onFrame func(png []byte)) (*Tab, error) {
	ctx, cancel := chromedp.NewContext(b.ctx)
	chromedp.ListenTarget(ctx, func(ev any) {
		f, ok := ev.(*page.EventScreencastFrame)
		if !ok {
			return
		}
		// Acked at once, whatever the panel is doing, so Chrome keeps painting this tab.
		go chromedp.Run(ctx, page.ScreencastFrameAck(f.SessionID))
		if png, err := base64.StdEncoding.DecodeString(f.Data); err == nil {
			onFrame(png)
		}
	})
	err := chromedp.Run(ctx,
		emulation.SetDeviceMetricsOverride(Width, Height, 1, false),
		emulation.SetFocusEmulationEnabled(true), // pages act as if focused, like the one visible tab
		page.StartScreencast().WithFormat(page.ScreencastFormatPng).WithMaxWidth(Width).WithMaxHeight(Height),
	)
	if err != nil {
		cancel()
		return nil, fmt.Errorf("opening a tab: %w", err)
	}
	return &Tab{ctx: ctx, cancel: cancel}, nil
}

// Navigate loads url and waits for its load event (at most 30 s). Chrome shows its own
// error page when the URL fails, and that page is pushed like any other.
func (t *Tab) Navigate(ctx context.Context, url string) error {
	return t.run(ctx, navigateTimeout, chromedp.Navigate(url))
}

// Reload reloads the page, bypassing the cache.
func (t *Tab) Reload(ctx context.Context) error {
	return t.run(ctx, navigateTimeout, page.Reload().WithIgnoreCache(true))
}

// Mouse dispatches one left-button mouse event at logical (x, y).
func (t *Tab) Mouse(ctx context.Context, typ input.MouseType, x, y float64) error {
	ev := input.DispatchMouseEvent(typ, x, y).WithButton(input.Left).WithClickCount(1)
	if typ != input.MouseReleased {
		ev = ev.WithButtons(1) // the left button is held
	}
	return t.run(ctx, 5*time.Second, ev)
}

// run executes actions in the tab, bounded by timeout and by ctx.
func (t *Tab) run(ctx context.Context, timeout time.Duration, actions ...chromedp.Action) error {
	tctx, cancel := context.WithTimeout(t.ctx, timeout)
	defer cancel()
	stop := context.AfterFunc(ctx, cancel)
	defer stop()
	err := chromedp.Run(tctx, actions...)
	if errors.Is(err, context.DeadlineExceeded) {
		return fmt.Errorf("Chrome did not finish within %v", timeout)
	}
	return err
}

// Close closes the tab.
func (t *Tab) Close() {
	ctx, cancel := context.WithTimeout(t.ctx, 3*time.Second)
	defer cancel()
	chromedp.Cancel(ctx)
	t.cancel()
}
